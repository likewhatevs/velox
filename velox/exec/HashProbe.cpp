/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/HashProbe.h"
#include "velox/exec/OperatorUtils.h"
#include "velox/exec/Task.h"
#include "velox/expression/FieldReference.h"

namespace facebook::velox::exec {

namespace {

// Batch size used when iterating the row container.
constexpr int kBatchSize = 1024;

// Returns the type for the hash table row. Build side keys first,
// then dependent build side columns.
RowTypePtr makeTableType(
    const RowType* type,
    const std::vector<std::shared_ptr<const core::FieldAccessTypedExpr>>&
        keys) {
  std::vector<std::string> names;
  std::vector<TypePtr> types;
  std::unordered_set<column_index_t> keyChannels(keys.size());
  names.reserve(type->size());
  types.reserve(type->size());
  for (const auto& key : keys) {
    auto channel = type->getChildIdx(key->name());
    names.emplace_back(type->nameOf(channel));
    types.emplace_back(type->childAt(channel));
    keyChannels.insert(channel);
  }
  for (auto i = 0; i < type->size(); ++i) {
    if (keyChannels.find(i) == keyChannels.end()) {
      names.emplace_back(type->nameOf(i));
      types.emplace_back(type->childAt(i));
    }
  }
  return ROW(std::move(names), std::move(types));
}

// Copy values from 'rows' of 'table' according to 'projections' in
// 'result'. Reuses 'result' children where possible.
void extractColumns(
    BaseHashTable* table,
    folly::Range<char**> rows,
    folly::Range<const IdentityProjection*> projections,
    memory::MemoryPool* pool,
    const RowVectorPtr& result) {
  for (auto projection : projections) {
    auto& child = result->childAt(projection.outputChannel);
    // TODO: Consider reuse of complex types.
    if (!child || !BaseVector::isVectorWritable(child) ||
        !child->isFlatEncoding()) {
      child = BaseVector::create(
          result->type()->childAt(projection.outputChannel), rows.size(), pool);
    }
    child->resize(rows.size());
    table->rows()->extractColumn(
        rows.data(), rows.size(), projection.inputChannel, child);
  }
}

folly::Range<vector_size_t*> initializeRowNumberMapping(
    BufferPtr& mapping,
    vector_size_t size,
    memory::MemoryPool* pool) {
  if (!mapping || !mapping->unique() ||
      mapping->size() < sizeof(vector_size_t) * size) {
    mapping = allocateIndices(size, pool);
  }
  return folly::Range(mapping->asMutable<vector_size_t>(), size);
}
} // namespace

HashProbe::HashProbe(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::HashJoinNode>& joinNode)
    : Operator(
          driverCtx,
          joinNode->outputType(),
          operatorId,
          joinNode->id(),
          "HashProbe"),
      outputBatchSize_{driverCtx->queryConfig().preferredOutputBatchSize()},
      joinType_{joinNode->joinType()},
      filterResult_(1),
      outputRows_(outputBatchSize_) {
  auto probeType = joinNode->sources()[0]->outputType();
  auto numKeys = joinNode->leftKeys().size();
  keyChannels_.reserve(numKeys);
  hashers_.reserve(numKeys);
  for (auto& key : joinNode->leftKeys()) {
    auto channel = exprToChannel(key.get(), probeType);
    keyChannels_.emplace_back(channel);
    hashers_.push_back(
        std::make_unique<VectorHasher>(probeType->childAt(channel), channel));
  }
  lookup_ = std::make_unique<HashLookup>(hashers_);
  auto buildType = joinNode->sources()[1]->outputType();
  auto tableType = makeTableType(buildType.get(), joinNode->rightKeys());
  if (joinNode->filter()) {
    initializeFilter(joinNode->filter(), probeType, tableType);
  }

  size_t countIdentityProjection = 0;
  for (auto i = 0; i < probeType->size(); ++i) {
    auto name = probeType->nameOf(i);
    auto outIndex = outputType_->getChildIdxIfExists(name);
    if (outIndex.has_value()) {
      identityProjections_.emplace_back(i, outIndex.value());
      if (outIndex.value() == i) {
        countIdentityProjection++;
      }
    }
  }

  for (column_index_t i = 0; i < outputType_->size(); ++i) {
    auto tableChannel = tableType->getChildIdxIfExists(outputType_->nameOf(i));
    if (tableChannel.has_value()) {
      tableResultProjections_.emplace_back(tableChannel.value(), i);
    }
  }

  if (countIdentityProjection == probeType->size() &&
      tableResultProjections_.empty()) {
    isIdentityProjection_ = true;
  }

  if (isNullAwareAntiJoin(joinType_)) {
    filterResultBuildSide_.resize(1);
  }
}

void HashProbe::initializeFilter(
    const core::TypedExprPtr& filter,
    const RowTypePtr& probeType,
    const RowTypePtr& tableType) {
  std::vector<core::TypedExprPtr> filters = {filter};
  filter_ =
      std::make_unique<ExprSet>(std::move(filters), operatorCtx_->execCtx());

  column_index_t filterChannel = 0;
  std::vector<std::string> names;
  std::vector<TypePtr> types;
  auto numFields = filter_->expr(0)->distinctFields().size();
  names.reserve(numFields);
  types.reserve(numFields);
  for (auto& field : filter_->expr(0)->distinctFields()) {
    const auto& name = field->field();
    auto channel = probeType->getChildIdxIfExists(name);
    if (channel.has_value()) {
      auto channelValue = channel.value();
      filterProbeInputs_.emplace_back(channelValue, filterChannel++);
      names.emplace_back(probeType->nameOf(channelValue));
      types.emplace_back(probeType->childAt(channelValue));
      continue;
    }
    channel = tableType->getChildIdxIfExists(name);
    if (channel.has_value()) {
      auto channelValue = channel.value();
      filterBuildInputs_.emplace_back(channelValue, filterChannel++);
      names.emplace_back(tableType->nameOf(channelValue));
      types.emplace_back(tableType->childAt(channelValue));
      continue;
    }
    VELOX_FAIL(
        "Join filter field {} not in probe or build input", field->toString());
  }

  filterInputType_ = ROW(std::move(names), std::move(types));
}

void HashProbe::prepareForNullAwareAntiJoinWithFilter() {
  for (auto& p : filterBuildInputs_) {
    filterBuildInputsMap_[p.inputChannel] = p.outputChannel;
  }
  filterInputBuildSide_ = std::static_pointer_cast<RowVector>(
      BaseVector::create(filterInputType_, kBatchSize, pool()));
}

BlockingReason HashProbe::isBlocked(ContinueFuture* future) {
  if (table_) {
    return BlockingReason::kNotBlocked;
  }

  auto hashBuildResult =
      operatorCtx_->task()
          ->getHashJoinBridge(
              operatorCtx_->driverCtx()->splitGroupId, planNodeId())
          ->tableOrFuture(future);
  if (!hashBuildResult.has_value()) {
    VELOX_CHECK_NOT_NULL(future);
    return BlockingReason::kWaitForJoinBuild;
  }

  if (hashBuildResult->antiJoinHasNullKeys) {
    // Anti join with null keys on the build side always returns nothing.
    VELOX_CHECK(isNullAwareAntiJoin(joinType_));
    finished_ = true;
  } else {
    table_ = hashBuildResult->table;
    if (table_->numDistinct() == 0) {
      // Build side is empty. Inner, right and semi joins return nothing in this
      // case, hence, we can terminate the pipeline early.
      if (isInnerJoin(joinType_) || isLeftSemiJoin(joinType_) ||
          isRightJoin(joinType_) || isRightSemiJoin(joinType_)) {
        finished_ = true;
      }
    } else if (
        (isInnerJoin(joinType_) || isLeftSemiJoin(joinType_) ||
         isRightSemiJoin(joinType_)) &&
        table_->hashMode() != BaseHashTable::HashMode::kHash) {
      // Find out whether there are any upstream operators that can accept
      // dynamic filters on all or a subset of the join keys. Create dynamic
      // filters to push down.
      const auto& buildHashers = table_->hashers();
      auto channels = operatorCtx_->driverCtx()->driver->canPushdownFilters(
          this, keyChannels_);
      for (auto i = 0; i < keyChannels_.size(); i++) {
        if (channels.find(keyChannels_[i]) != channels.end()) {
          if (auto filter = buildHashers[i]->getFilter(false)) {
            dynamicFilters_.emplace(keyChannels_[i], std::move(filter));
          }
        }
      }
    }
    if (isNullAwareAntiJoin(joinType_) && filter_) {
      prepareForNullAwareAntiJoinWithFilter();
    }
  }

  return BlockingReason::kNotBlocked;
}

void HashProbe::clearDynamicFilters() {
  // The join can be completely replaced with a pushed down
  // filter when the following conditions are met:
  //  * hash table has a single key with unique values,
  //  * build side has no dependent columns.
  if (keyChannels_.size() == 1 && !table_->hasDuplicateKeys() &&
      tableResultProjections_.empty() && !filter_ && !dynamicFilters_.empty()) {
    canReplaceWithDynamicFilter_ = true;
  }

  Operator::clearDynamicFilters();
}

void HashProbe::addInput(RowVectorPtr input) {
  input_ = std::move(input);

  if (canReplaceWithDynamicFilter_) {
    replacedWithDynamicFilter_ = true;
    return;
  }

  if (table_->numDistinct() == 0) {
    // Build side is empty. This state is valid only for anti, left and full
    // joins.
    VELOX_CHECK(
        isNullAwareAntiJoin(joinType_) || isLeftJoin(joinType_) ||
        isFullJoin(joinType_));
    return;
  }

  nonNullRows_.resize(input_->size());
  nonNullRows_.setAll();

  for (auto i = 0; i < hashers_.size(); ++i) {
    auto key = input_->childAt(hashers_[i]->channel())->loadedVector();
    hashers_[i]->decode(*key, nonNullRows_);
  }

  deselectRowsWithNulls(hashers_, nonNullRows_);

  activeRows_ = nonNullRows_;
  lookup_->hashes.resize(input_->size());
  auto mode = table_->hashMode();
  auto& buildHashers = table_->hashers();
  for (auto i = 0; i < keyChannels_.size(); ++i) {
    if (mode != BaseHashTable::HashMode::kHash) {
      auto key = input_->childAt(keyChannels_[i]);
      buildHashers[i]->lookupValueIds(
          *key, activeRows_, scratchMemory_, lookup_->hashes);
    } else {
      hashers_[i]->hash(activeRows_, i > 0, lookup_->hashes);
    }
  }
  lookup_->rows.clear();
  if (activeRows_.isAllSelected()) {
    lookup_->rows.resize(activeRows_.size());
    std::iota(lookup_->rows.begin(), lookup_->rows.end(), 0);
  } else {
    bits::forEachSetBit(
        activeRows_.asRange().bits(),
        0,
        activeRows_.size(),
        [&](vector_size_t row) { lookup_->rows.push_back(row); });
  }

  passingInputRowsInitialized_ = false;
  if (isLeftJoin(joinType_) || isFullJoin(joinType_) ||
      isNullAwareAntiJoin(joinType_)) {
    // Make sure to allocate an entry in 'hits' for every input row to allow for
    // including rows without a match in the output. Also, make sure to
    // initialize all 'hits' to nullptr as HashTable::joinProbe will only
    // process activeRows_.
    auto numInput = input_->size();
    auto& hits = lookup_->hits;
    hits.resize(numInput);
    std::fill(hits.data(), hits.data() + numInput, nullptr);
    if (!lookup_->rows.empty()) {
      table_->joinProbe(*lookup_);
    }

    // Update lookup_->rows to include all input rows, not just activeRows_ as
    // we need to include all rows in the output.
    auto& rows = lookup_->rows;
    rows.resize(numInput);
    std::iota(rows.begin(), rows.end(), 0);
  } else {
    if (lookup_->rows.empty()) {
      input_ = nullptr;
      return;
    }
    lookup_->hits.resize(lookup_->rows.back() + 1);
    table_->joinProbe(*lookup_);
  }
  results_.reset(*lookup_);
}

void HashProbe::prepareOutput(vector_size_t size) {
  // Try to re-use memory for the output vectors that contain build-side data.
  // We expect output vectors containing probe-side data to be null (reset in
  // clearIdentityProjectedOutput). BaseVector::prepareForReuse keeps null
  // children unmodified and makes non-null (build side) children reusable.
  if (output_) {
    VectorPtr output = std::move(output_);
    BaseVector::prepareForReuse(output, size);
    output_ = std::static_pointer_cast<RowVector>(output);
  } else {
    output_ = std::static_pointer_cast<RowVector>(
        BaseVector::create(outputType_, size, pool()));
  }
}

void HashProbe::fillOutput(vector_size_t size) {
  prepareOutput(size);

  for (auto projection : identityProjections_) {
    // Load input vector if it is being split into multiple batches. It is not
    // safe to wrap unloaded LazyVector into two different dictionaries.
    ensureLoadedIfNotAtEnd(projection.inputChannel);
    auto inputChild = input_->childAt(projection.inputChannel);

    output_->childAt(projection.outputChannel) =
        wrapChild(size, rowNumberMapping_, inputChild);
  }

  extractColumns(
      table_.get(),
      folly::Range<char**>(outputRows_.data(), size),
      tableResultProjections_,
      pool(),
      output_);
}

RowVectorPtr HashProbe::getBuildSideOutput() {
  outputRows_.resize(outputBatchSize_);
  int32_t numOut;
  if (isRightSemiJoin(joinType_)) {
    numOut = table_->listProbedRows(
        &lastProbeIterator_,
        outputBatchSize_,
        RowContainer::kUnlimited,
        outputRows_.data());
  } else {
    // Must be a right join or full join.
    numOut = table_->listNotProbedRows(
        &lastProbeIterator_,
        outputBatchSize_,
        RowContainer::kUnlimited,
        outputRows_.data());
  }
  if (!numOut) {
    return nullptr;
  }

  prepareOutput(numOut);

  // Populate probe-side columns of the output with nulls.
  for (auto projection : identityProjections_) {
    output_->childAt(projection.outputChannel) = BaseVector::createNullConstant(
        outputType_->childAt(projection.outputChannel), numOut, pool());
  }

  extractColumns(
      table_.get(),
      folly::Range<char**>(outputRows_.data(), numOut),
      tableResultProjections_,
      pool(),
      output_);
  return output_;
}

void HashProbe::clearIdentityProjectedOutput() {
  if (!output_ || !output_.unique()) {
    return;
  }
  for (auto& projection : identityProjections_) {
    output_->childAt(projection.outputChannel) = nullptr;
  }
}

RowVectorPtr HashProbe::getOutput() {
  clearIdentityProjectedOutput();
  if (!input_) {
    if (noMoreInput_ &&
        (isRightJoin(joinType_) || isFullJoin(joinType_) ||
         isRightSemiJoin(joinType_)) &&
        lastProbe_) {
      auto output = getBuildSideOutput();
      if (output == nullptr) {
        finished_ = true;
      }
      return output;
    }
    if (noMoreInput_) {
      finished_ = true;
    }
    return nullptr;
  }

  const auto inputSize = input_->size();

  if (replacedWithDynamicFilter_) {
    stats_.addRuntimeStat(
        "replacedWithDynamicFilterRows", RuntimeCounter(inputSize));
    auto output = Operator::fillOutput(inputSize, nullptr);
    input_ = nullptr;
    return output;
  }

  const bool isLeftSemiOrAntiJoinNoFilter = !filter_ &&
      (core::isLeftSemiJoin(joinType_) || core::isNullAwareAntiJoin(joinType_));

  const bool emptyBuildSide = (table_->numDistinct() == 0);

  // Left semi and anti joins are always cardinality reducing, e.g. for a given
  // row of input they produce zero or 1 row of output. Therefore, if there is
  // no extra filter we can process each batch of input in one go.
  auto outputBatchSize = (isLeftSemiOrAntiJoinNoFilter || emptyBuildSide)
      ? inputSize
      : outputBatchSize_;
  auto mapping =
      initializeRowNumberMapping(rowNumberMapping_, outputBatchSize, pool());
  outputRows_.resize(outputBatchSize);

  for (;;) {
    int numOut = 0;

    if (emptyBuildSide) {
      // When build side is empty, anti and left joins return all probe side
      // rows, including ones with null join keys.
      std::iota(mapping.begin(), mapping.end(), 0);
      numOut = inputSize;
    } else if (isNullAwareAntiJoin(joinType_) && !filter_) {
      // When build side is not empty, anti join without a filter returns probe
      // rows with no nulls in the join key and no match in the build side.
      for (auto i = 0; i < inputSize; i++) {
        if (nonNullRows_.isValid(i) &&
            (!activeRows_.isValid(i) || !lookup_->hits[i])) {
          mapping[numOut] = i;
          ++numOut;
        }
      }
    } else {
      numOut = table_->listJoinResults(
          results_,
          isLeftJoin(joinType_) || isFullJoin(joinType_) ||
              isNullAwareAntiJoin(joinType_),
          mapping,
          folly::Range(outputRows_.data(), outputRows_.size()));
    }

    if (!numOut) {
      input_ = nullptr;
      return nullptr;
    }
    VELOX_CHECK_LE(numOut, outputRows_.size());

    numOut = evalFilter(numOut);
    if (!numOut) {
      // The filter was false on all rows.
      if (isLeftSemiOrAntiJoinNoFilter) {
        input_ = nullptr;
        return nullptr;
      }
      continue;
    }

    if (isRightJoin(joinType_) || isFullJoin(joinType_) ||
        isRightSemiJoin(joinType_)) {
      // Mark build-side rows that have a match on the join condition.
      table_->rows()->setProbedFlag(outputRows_.data(), numOut);
    }

    // Right semi join only returns the build side output when the probe side
    // is fully complete. Do not return anything here.
    if (isRightSemiJoin(joinType_)) {
      if (results_.atEnd()) {
        input_ = nullptr;
      }
      return nullptr;
    }

    fillOutput(numOut);

    if (isLeftSemiOrAntiJoinNoFilter || emptyBuildSide) {
      input_ = nullptr;
    }
    return output_;
  }
}

void HashProbe::fillFilterInput(vector_size_t size) {
  if (!filterInput_) {
    filterInput_ = std::static_pointer_cast<RowVector>(
        BaseVector::create(filterInputType_, 1, pool()));
  }
  filterInput_->resize(size);
  for (auto projection : filterProbeInputs_) {
    ensureLoadedIfNotAtEnd(projection.inputChannel);
    filterInput_->childAt(projection.outputChannel) = wrapChild(
        size, rowNumberMapping_, input_->childAt(projection.inputChannel));
  }

  extractColumns(
      table_.get(),
      folly::Range<char**>(outputRows_.data(), size),
      filterBuildInputs_,
      pool(),
      filterInput_);
}

void HashProbe::prepareFilterRowsForNullAwareAntiJoin(
    const Expr& filter,
    vector_size_t numRows) {
  if (filter.propagatesNulls()) {
    nullFilterProbeInputRows_.resizeFill(numRows, false);
    auto nullRows = nullFilterProbeInputRows_.asMutableRange().bits();
    for (auto& p : filterProbeInputs_) {
      decodedVectorPerRow_.decode(
          *filterInput_->childAt(p.outputChannel), filterRows_);
      if (decodedVectorPerRow_.mayHaveNulls()) {
        bits::orWithNegatedBits(
            nullRows, decodedVectorPerRow_.nulls(), 0, numRows);
      }
    }
    nullFilterProbeInputRows_.updateBounds();
  }
  if (!nonNullRows_.isAllSelected()) {
    auto rawMapping = rowNumberMapping_->asMutable<vector_size_t>();
    for (int i = 0; i < numRows; ++i) {
      filterRows_.setValid(i, nonNullRows_.isValid(rawMapping[i]));
    }
    filterRows_.updateBounds();
  }
}

void HashProbe::testFilterOnBuildSide(
    SelectivityVector& rows,
    bool nullKeyRowsOnly) {
  if (!rows.hasSelections()) {
    return;
  }
  auto tableRows = table_->rows();
  if (!tableRows) {
    return;
  }
  RowContainerIterator iter;
  char* data[kBatchSize];
  while (auto numRows = tableRows->listRows(
             &iter, kBatchSize, RowContainer::kUnlimited, data)) {
    filterInputBuildSide_->resize(numRows);
    filterRowsBuildSide_.resizeFill(numRows, true);
    auto nonNullRows = filterRowsBuildSide_.asMutableRange().bits();
    for (column_index_t j = 0; j < tableRows->columnTypes().size(); ++j) {
      VectorPtr c;
      if (auto it = filterBuildInputsMap_.find(j);
          it != filterBuildInputsMap_.end()) {
        c = filterInputBuildSide_->childAt(it->second);
        tableRows->extractColumn(data, numRows, j, c);
      }
      if (nullKeyRowsOnly && j < tableRows->keyTypes().size()) {
        if (!c) {
          c = BaseVector::create(tableRows->keyTypes()[j], numRows, pool());
          tableRows->extractColumn(data, numRows, j, c);
        }
        decodedVectorPerRow_.decode(*c, filterRowsBuildSide_);
        if (decodedVectorPerRow_.mayHaveNulls()) {
          bits::andBits(nonNullRows, decodedVectorPerRow_.nulls(), 0, numRows);
        }
      }
    }
    if (nullKeyRowsOnly) {
      bits::negate(reinterpret_cast<char*>(nonNullRows), numRows);
      filterRowsBuildSide_.updateBounds();
    }
    rows.applyToSelected([&](vector_size_t i) {
      for (auto& p : filterProbeInputs_) {
        filterInputBuildSide_->childAt(p.outputChannel) =
            BaseVector::wrapInConstant(
                numRows, i, input_->childAt(p.inputChannel));
      }
      EvalCtx evalCtx(
          operatorCtx_->execCtx(), filter_.get(), filterInputBuildSide_.get());
      filter_->eval(filterRowsBuildSide_, evalCtx, filterResultBuildSide_);
      decodedVectorPerRow_.decode(
          *filterResultBuildSide_[0], filterRowsBuildSide_);
      bool passed = !filterRowsBuildSide_.testSelected([&](vector_size_t j) {
        return decodedVectorPerRow_.isNullAt(j) ||
            !decodedVectorPerRow_.valueAt<bool>(j);
      });
      if (passed) {
        rows.setValid(i, false);
      }
    });
  }
  rows.updateBounds();
}

vector_size_t HashProbe::evalFilterInNullAwareAntiJoin(
    vector_size_t numRows,
    const Expr& filter) {
  auto rawMapping = rowNumberMapping_->asMutable<vector_size_t>();
  SelectivityVector skipRows(numRows, false);
  SelectivityVector testNullKeyRows(input_->size(), false);
  SelectivityVector testAllRows(input_->size(), false);
  for (auto i = 0; i < numRows; ++i) {
    auto j = rawMapping[i];
    if (filter.propagatesNulls() && nullFilterProbeInputRows_.isValid(i)) {
      skipRows.setValid(i, true);
    } else if (nonNullRows_.isValid(j)) {
      if (!decodedFilterResult_.isNullAt(i) &&
          decodedFilterResult_.valueAt<bool>(i)) {
        skipRows.setValid(i, true);
      } else {
        testNullKeyRows.setValid(j, true);
      }
    } else {
      testAllRows.setValid(j, true);
    }
  }
  skipRows.updateBounds();
  skipRows.applyToSelected([&](vector_size_t i) {
    auto j = rawMapping[i];
    testNullKeyRows.setValid(j, false);
    testAllRows.setValid(j, false);
  });
  testNullKeyRows.updateBounds();
  testFilterOnBuildSide(testNullKeyRows, true);
  testAllRows.updateBounds();
  testFilterOnBuildSide(testAllRows, false);
  vector_size_t numPassed = 0;
  auto addMiss = [&](auto row) {
    outputRows_[numPassed] = nullptr;
    rawMapping[numPassed++] = row;
  };
  for (auto i = 0; i < numRows; ++i) {
    auto j = rawMapping[i];
    bool passed;
    if (filter.propagatesNulls() && nullFilterProbeInputRows_.isValid(i)) {
      passed = false;
    } else if (nonNullRows_.isValid(j)) {
      if (!decodedFilterResult_.isNullAt(i) &&
          decodedFilterResult_.valueAt<bool>(i)) {
        passed = true;
      } else {
        passed = !testNullKeyRows.isValid(j);
      }
    } else {
      passed = !testAllRows.isValid(j);
    }
    noMatchDetector_.advance(j, passed, addMiss);
  }
  if (results_.atEnd()) {
    noMatchDetector_.finish(addMiss);
  }
  return numPassed;
}

int32_t HashProbe::evalFilter(int32_t numRows) {
  if (!filter_) {
    return numRows;
  }
  auto& filter = filter_->expr(0);
  auto rawMapping = rowNumberMapping_->asMutable<vector_size_t>();

  fillFilterInput(numRows);
  filterRows_.resizeFill(numRows);
  if (isNullAwareAntiJoin(joinType_)) {
    prepareFilterRowsForNullAwareAntiJoin(*filter, numRows);
  }

  EvalCtx evalCtx(operatorCtx_->execCtx(), filter_.get(), filterInput_.get());
  filter_->eval(0, 1, true, filterRows_, evalCtx, filterResult_);

  decodedFilterResult_.decode(*filterResult_[0], filterRows_);

  int32_t numPassed = 0;
  if (isLeftJoin(joinType_) || isFullJoin(joinType_)) {
    // Identify probe rows which got filtered out and add them back with nulls
    // for build side.
    auto addMiss = [&](auto row) {
      outputRows_[numPassed] = nullptr;
      rawMapping[numPassed++] = row;
    };
    for (auto i = 0; i < numRows; ++i) {
      const bool passed = !decodedFilterResult_.isNullAt(i) &&
          decodedFilterResult_.valueAt<bool>(i);
      noMatchDetector_.advance(rawMapping[i], passed, addMiss);
      if (passed) {
        outputRows_[numPassed] = outputRows_[i];
        rawMapping[numPassed++] = rawMapping[i];
      }
    }
    if (results_.atEnd()) {
      noMatchDetector_.finish(addMiss);
    }
  } else if (isLeftSemiJoin(joinType_)) {
    auto addLastMatch = [&](auto row) {
      outputRows_[numPassed] = nullptr;
      rawMapping[numPassed++] = row;
    };
    for (auto i = 0; i < numRows; ++i) {
      if (!decodedFilterResult_.isNullAt(i) &&
          decodedFilterResult_.valueAt<bool>(i)) {
        leftSemiJoinTracker_.advance(rawMapping[i], addLastMatch);
      }
    }
    if (results_.atEnd()) {
      leftSemiJoinTracker_.finish(addLastMatch);
    }
  } else if (isNullAwareAntiJoin(joinType_)) {
    numPassed = evalFilterInNullAwareAntiJoin(numRows, *filter);
  } else {
    for (auto i = 0; i < numRows; ++i) {
      if (!decodedFilterResult_.isNullAt(i) &&
          decodedFilterResult_.valueAt<bool>(i)) {
        outputRows_[numPassed] = outputRows_[i];
        rawMapping[numPassed++] = rawMapping[i];
      }
    }
  }
  return numPassed;
}

void HashProbe::ensureLoadedIfNotAtEnd(column_index_t channel) {
  if (core::isLeftSemiJoin(joinType_) || core::isNullAwareAntiJoin(joinType_) ||
      results_.atEnd()) {
    return;
  }

  if (!passingInputRowsInitialized_) {
    passingInputRowsInitialized_ = true;
    passingInputRows_.resize(input_->size());
    if (isLeftJoin(joinType_) || isFullJoin(joinType_)) {
      passingInputRows_.setAll();
    } else {
      passingInputRows_.clearAll();
      auto hitsSize = lookup_->hits.size();
      auto hits = lookup_->hits.data();
      for (auto i = 0; i < hitsSize; ++i) {
        if (hits[i]) {
          passingInputRows_.setValid(i, true);
        }
      }
    }
    passingInputRows_.updateBounds();
  }

  LazyVector::ensureLoadedRows(input_->childAt(channel), passingInputRows_);
}

void HashProbe::noMoreInput() {
  Operator::noMoreInput();
  if (isRightJoin(joinType_) || isFullJoin(joinType_) ||
      isRightSemiJoin(joinType_)) {
    std::vector<ContinuePromise> promises;
    std::vector<std::shared_ptr<Driver>> peers;
    // The last Driver to hit HashProbe::finish is responsible for producing
    // build-side rows based on the join.
    ContinueFuture future;
    if (!operatorCtx_->task()->allPeersFinished(
            planNodeId(), operatorCtx_->driver(), &future, promises, peers)) {
      return;
    }

    lastProbe_ = true;
  }
}

bool HashProbe::isFinished() {
  return finished_;
}
} // namespace facebook::velox::exec
