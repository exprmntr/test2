#pragma once

#include "features.h"
#include "split.h"

#include "static_ctr_provider.h"

#include <catboost/libs/model/flatbuffers/model.fbs.h>

#include <catboost/libs/cat_feature/cat_feature.h>

#include <library/json/json_reader.h>

#include <util/system/mutex.h>
#include <util/stream/file.h>

class TModelPartsCachingSerializer;

/*!
    \brief Oblivious tree model structure

    This structure contains the data about tree conditions and leaf values.
    We use oblivious trees - symmetric trees that has the same binary condition on each level.
    So each leaf index is determined by binary vector with length equal to evaluated tree depth.

    That allows us to evaluate model predictions very fast (even without planned SIMD optimizations) compared to asymmetric trees.

    Our oblivious tree model can contain float, one-hot and CTR binary conditions:
    - Float condition - float feature value is greater than float border
    - One-hot condition - hashed cat feature value is equal to some value
    - CTR condition - calculated ctr is greater than float border
    You can read about CTR calculation in ctr_provider.h

    FloatFeatures, OneHotFeatures and CtrFeatures form binary features(or binary conditions) sequence.
    Information about tree structure is stored in 3 integer vectors:
    TreeSplits, TreeSizes, TreeStartOffsets.
    - TreeSplits - holds all binary feature indexes from all the trees.
    - TreeSizes - holds tree depth.
    - TreeStartOffsets - holds offset of first tree split in TreeSplits vector
*/

struct TObliviousTrees {

    /**
     * This structure stores model metadata. Should be kept up to date
     */
    struct TMetaData {
        /**
         * List of all TModelCTR used in model
         */
        TVector<TModelCtr> UsedModelCtrs;
        /**
         * List of all binary with indexes corresponding to TreeSplits values
         */
        TVector<TModelSplit> BinFeatures;

        /**
        * This vector containes ui32 that contains such information:
        * |     ui16     |   ui8   |   ui8  |
        * | featureIndex | xorMask |splitIdx| (e.g. featureIndex << 16 + xorMask << 8 + splitIdx )
        *
        * We use this layout to speed up model apply - we only need to store one byte for each float, ctr or one hot feature.
        * TODO(kirillovs): Currently we don't support models with more than 255 splits for a feature, but this will be fixed soon.
        */
        TVector<ui32> RepackedBins;

        ui32 EffectiveBinFeaturesBucketCount = 0;
    };

    //! Number of classes in model, in most cases equals to 1.
    int ApproxDimension = 1;

    //! Split values
    TVector<int> TreeSplits;

    //! Tree sizes
    TVector<int> TreeSizes;

    //! Offset of first split in TreeSplits array
    TVector<int> TreeStartOffsets;

    //! Leaf values layout: [treeIndex][leafId * ApproxDimension + dimension]
    TVector<TVector<double>> LeafValues;

    //! Categorical features, used in model in OneHot conditions or/and in CTR feature combinations
    TVector<TCatFeature> CatFeatures;

    static_assert(ESplitType::FloatFeature < ESplitType::OneHotFeature
                  && ESplitType::OneHotFeature < ESplitType::OnlineCtr,
                  "ESplitType should represent bin feature order in model");

    //! Float features used in model
    TVector<TFloatFeature> FloatFeatures;
    //! One hot encoded features used in model
    TVector<TOneHotFeature> OneHotFeatures;
    //! CTR features used in model
    TVector<TCtrFeature> CtrFeatures;

    /**
     * Method for oblivious trees serialization with repeated parts caching
     * @param serializer our caching flatbuffers serializator
     * @return offset in flatbuffer
     */
    flatbuffers::Offset<NCatBoostFbs::TObliviousTrees> FBSerialize(TModelPartsCachingSerializer& serializer) const;

    /**
     * Deserialize from flatbuffers object
     * @param fbObj
     */
    void FBDeserialize(const NCatBoostFbs::TObliviousTrees* fbObj) {
        ApproxDimension = fbObj->ApproxDimension();
        if (fbObj->TreeSplits()) {
            TreeSplits.assign(fbObj->TreeSplits()->begin(), fbObj->TreeSplits()->end());
        }
        if (fbObj->TreeSizes()) {
            TreeSizes.assign(fbObj->TreeSizes()->begin(), fbObj->TreeSizes()->end());
        }
        if (fbObj->TreeStartOffsets()) {
            TreeStartOffsets.assign(fbObj->TreeStartOffsets()->begin(), fbObj->TreeStartOffsets()->end());
        }
        LeafValues.resize(TreeSizes.size());
        if (fbObj->LeafValues()) {
            auto leafValIter = fbObj->LeafValues()->begin();
            for (size_t treeId = 0; treeId < TreeSizes.size(); ++treeId) {
                const auto treeLeafCout = ApproxDimension * (1 << TreeSizes[treeId]);
                LeafValues[treeId].assign(leafValIter, leafValIter + treeLeafCout);
                leafValIter += treeLeafCout;
            }
        }

#define FEATURES_ARRAY_DESERIALIZER(var) \
        if (fbObj->var()) {\
            var.resize(fbObj->var()->size());\
            for (size_t i = 0; i < fbObj->var()->size(); ++i) {\
                var[i].FBDeserialize(fbObj->var()->Get(i));\
            }\
        }
        FEATURES_ARRAY_DESERIALIZER(CatFeatures)
        FEATURES_ARRAY_DESERIALIZER(FloatFeatures)
        FEATURES_ARRAY_DESERIALIZER(OneHotFeatures)
        FEATURES_ARRAY_DESERIALIZER(CtrFeatures)
#undef FEATURES_ARRAY_DESERIALIZER
    }
    /**
     * Internal usage only. Insert binary conditions tree with proper TreeSizes and TreeStartOffsets modification
     * @param binSplits
     */
    void AddBinTree(const TVector<int>& binSplits) {
        Y_ASSERT(TreeSplits.size() == TreeSizes.size() && TreeSizes.size() == TreeStartOffsets.size());
        TreeSplits.insert(TreeSplits.end(), binSplits.begin(), binSplits.end());
        TreeSizes.push_back(binSplits.ysize());
        if (TreeStartOffsets.empty()) {
            TreeStartOffsets.push_back(0);
        } else {
            TreeStartOffsets.push_back(TreeStartOffsets.back() + binSplits.ysize());
        }
    }

    bool operator==(const TObliviousTrees& other) const {
        return std::tie(ApproxDimension,
                        TreeSplits,
                        TreeSizes,
                        TreeStartOffsets,
                        LeafValues,
                        CatFeatures,
                        FloatFeatures,
                        OneHotFeatures,
                        CtrFeatures)
           == std::tie(other.ApproxDimension,
                       other.TreeSplits,
                       other.TreeSizes,
                       other.TreeStartOffsets,
                       other.LeafValues,
                       other.CatFeatures,
                       other.FloatFeatures,
                       other.OneHotFeatures,
                       other.CtrFeatures);
    }
    bool operator!=(const TObliviousTrees& other) const {
        return !(*this == other);
    }
    size_t GetTreeCount() const {
        return TreeSizes.size();
    }
    /**
     * Truncate oblivous trees to contain only trees from [begin; end) interval.
     * @param begin
     * @param end
     */
    void Truncate(size_t begin, size_t end);
    /**
     * Internal usage only. Updates metadata UsedModelCtrs and BinFeatures vectors to contain all features currently used in model.
     * Should be called after any modifications.
     */
    void UpdateMetadata() const;
    /**
     * List of all CTRs in model
     * @return
     */
    const TVector<TModelCtr>& GetUsedModelCtrs() const {
        Y_ENSURE(MetaData.Defined(), "metadata should be initialized");
        return MetaData->UsedModelCtrs;
    }
    /**
     * List all binary features corresponding to binary feature indexes in trees
     * @return
     */
    const TVector<TModelSplit>& GetBinFeatures() const {
        Y_ENSURE(MetaData.Defined(), "metadata should be initialized");
        return MetaData->BinFeatures;
    }

    const TVector<ui32>& GetRepackedBins() const {
        Y_ENSURE(MetaData.Defined(), "metadata should be initialized");
        return MetaData->RepackedBins;
    }

    /**
     * List all unique CTR bases (feature combination + ctr type) in model
     * @return
     */
    TVector<TModelCtrBase> GetUsedModelCtrBases() const {
        THashSet<TModelCtrBase> ctrsSet; // return sorted bases
        for (const auto& usedCtr : GetUsedModelCtrs()) {
            ctrsSet.insert(usedCtr.Base);
        }
        return TVector<TModelCtrBase>(ctrsSet.begin(), ctrsSet.end());
    }

    size_t GetNumFloatFeatures() const {
        if (FloatFeatures.empty()) {
            return 0;
        } else {
            return static_cast<size_t>(FloatFeatures.back().FeatureIndex + 1);
        }
    }

    size_t GetNumCatFeatures() const {
        if (CatFeatures.empty()) {
            return 0;
        } else {
            return static_cast<size_t>(CatFeatures.back().FeatureIndex + 1);
        }
    }

    size_t GetBinaryFeaturesFullCount() const {
        return GetBinFeatures().size();
    }

    ui32 GetEffectiveBinaryFeaturesBucketsCount() const {
        Y_ENSURE(MetaData.Defined(), "metadata should be initialized");
        return MetaData->EffectiveBinFeaturesBucketCount;
    }

    size_t GetFlatFeatureVectorExpectedSize() const {
        return GetNumFloatFeatures() + GetNumCatFeatures();
    }
private:
    mutable TMaybe<TMetaData> MetaData;
};

/*!
 * \brief Full model class - contains all the data for model evaluation
 *
 * This class contains oblivious trees data, key-value dictionary for model metadata storage and CtrProvider holder.
 */
struct TFullModel {
    TObliviousTrees ObliviousTrees;
    /**
     * Model information key-value storage.
     */
    THashMap<TString, TString> ModelInfo;
    TIntrusivePtr<ICtrProvider> CtrProvider;

    void Swap(TFullModel& other) {
        DoSwap(ObliviousTrees, other.ObliviousTrees);
        DoSwap(ModelInfo, other.ModelInfo);
        DoSwap(CtrProvider, other.CtrProvider);
    }

    /**
     * Check whether model contains categorical features in OneHot conditions and/or CTR feature combinations
     */
    bool HasCategoricalFeatures() const {
        return !ObliviousTrees.CatFeatures.empty();
    }

    size_t GetTreeCount() const {
        return ObliviousTrees.TreeSizes.size();
    }
    /**
     * @return Minimal float features vector length for this model
     */
    size_t GetNumFloatFeatures() const {
        return ObliviousTrees.GetNumFloatFeatures();
    }

    /**
     * @return Minimal categorical features vector length for this model
     */
    size_t GetNumCatFeatures() const {
        return ObliviousTrees.GetNumCatFeatures();
    }

    TFullModel() = default;

    bool operator==(const TFullModel& other) const {
        return std::tie(ObliviousTrees, ModelInfo) ==
               std::tie(other.ObliviousTrees, other.ModelInfo);
    }

    bool operator!=(const TFullModel& other) const {
        return !(*this == other);
    }
    /**
     * Serialize model to stream
     * @param s IOutputStream ptr
     */
    void Save(IOutputStream* s) const;
    /**
     * Deserialize model from stream
     * @param s IInputStream ptr
     */
    void Load(IInputStream* s);

    //! Check if TFullModel instance has valid CTR provider. If no ctr features present it will also return false
    bool HasValidCtrProvider() const {
        if (!CtrProvider) {
            return false;
        }
        return CtrProvider->HasNeededCtrs(ObliviousTrees.GetUsedModelCtrs());
    }

    /**
     * Special interface for model evaluation on transposed TPool layout
     * @param[in] transposedFeatures transposed flat features vector. First dimension is feature index, second dimension is object index.
     * If feature is categorical, we do reinterpret cast from float to int.
     * @param[in] treeStart Index of first tree in model to start evaluation
     * @param[in] treeEnd Index of tree after the last tree in model to evaluate. F.e. if you want to evaluate trees 2..5 use treeStart = 2, treeEnd = 6
     * @param[out] results Flat double vector with indexation [objectIndex * ApproxDimension + classId].
     * For single class models it is just [objectIndex]
     */
    void CalcFlatTransposed(const TVector<TConstArrayRef<float>>& transposedFeatures, size_t treeStart, size_t treeEnd, TArrayRef<double> results) const;
    /**
     * Special interface for model evaluation on flat feature vectors. Flat here means that float features and categorical feature are in the same float array.
     * @param[in] features vector of flat features array reference. First dimension is object index, second dimension is feature index.
     * If feature is categorical, we do reinterpret cast from float to int.
     * @param[in] treeStart Index of first tree in model to start evaluation
     * @param[in] treeEnd Index of tree after the last tree in model to evaluate. F.e. if you want to evaluate trees 2..5 use treeStart = 2, treeEnd = 6
     * @param[out] results Flat double vector with indexation [objectIndex * ApproxDimension + classId].
     * For single class models it is just [objectIndex]
     */

    void CalcFlat(const TVector<TConstArrayRef<float>>& features, size_t treeStart, size_t treeEnd, TArrayRef<double> results) const;
    /**
     * Call CalcFlat on all model trees
     * @param features
     * @param results
     */
    void CalcFlat(const TVector<TConstArrayRef<float>>& features, TArrayRef<double> results) const {
        CalcFlat(features, 0, ObliviousTrees.TreeSizes.size(), results);
    }
    /**
     * Same as CalcFlat method but for one object
     * @param[in] features flat features array reference. First dimension is object index, second dimension is feature index.
     * If feature is categorical, we do reinterpret cast from float to int.
     * @param[in] treeStart Index of first tree in model to start evaluation
     * @param[in] treeEnd Index of tree after the last tree in model to evaluate. F.e. if you want to evaluate trees 2..5 use treeStart = 2, treeEnd = 6
     * @param[out] results double vector with indexation [classId].
     */
    void CalcFlatSingle(const TConstArrayRef<float>& features, size_t treeStart, size_t treeEnd, TArrayRef<double> results) const;
    /**
     * CalcFlatSingle on all trees in the model
     * @param[in] features flat features array reference. First dimension is object index, second dimension is feature index.
     * If feature is categorical, we do reinterpret cast from float to int.
     * @param[out] results double vector with indexation [classId].
     */
    void CalcFlatSingle(const TConstArrayRef<float>& features, TArrayRef<double> results) const {
        CalcFlatSingle(features, 0, ObliviousTrees.TreeSizes.size(), results);
    }
    /**
     * Shortcut for CalcFlatSingle
     */
    void CalcFlat(TConstArrayRef<float> features, TArrayRef<double> result) const {
        CalcFlatSingle(features, result);
    }
    /**
     * Staged model evaluation. Evaluates model for each incrementStep trees.
     * Useful for per tree model quality analysis.
     * @param[in] floatFeatures vector of float features values array references
     * @param[in] catFeatures vector of hashed categorical features values array references
     * @param[in] incrementStep tree count on each prediction stage
     * @return vector of vector of double - first index is for stage id, second is for [objectIndex * ApproxDimension + classId]
     */
    TVector<TVector<double>> CalcTreeIntervals(
        const TVector<TConstArrayRef<float>>& floatFeatures,
        const TVector<TConstArrayRef<int>>& catFeatures,
        size_t incrementStep) const;
    /**
     * Same as CalcTreeIntervalsFlat but for **flat** feature vectors
     * @param[in] mixedFeatures
     * @param[in] incrementStep
     * @return
     */
    TVector<TVector<double>> CalcTreeIntervalsFlat(
        const TVector<TConstArrayRef<float>>& mixedFeatures,
        size_t incrementStep) const;
    /**
     * Evaluate raw formula predictions on user data. Uses model trees for interval [treeStart, treeEnd)
     * @param[in] floatFeatures
     * @param[in] catFeatures hashed cat feature values
     * @param[in] treeStart
     * @param[in] treeEnd
     * @param[out] results results indexation is [objectIndex * ApproxDimension + classId]
     */
    void Calc(const TVector<TConstArrayRef<float>>& floatFeatures,
              const TVector<TConstArrayRef<int>>& catFeatures,
              size_t treeStart,
              size_t treeEnd,
              TArrayRef<double> results) const;
    /**
     * Evaluate raw formula predictions on user data. Uses all model trees
     * @param floatFeatures
     * @param catFeatures hashed cat feature values
     * @param results results indexation is [objectIndex * ApproxDimension + classId]
     */
    void Calc(const TVector<TConstArrayRef<float>>& floatFeatures,
              const TVector<TConstArrayRef<int>>& catFeatures,
              TArrayRef<double> results) const {
        Calc(floatFeatures, catFeatures, 0, ObliviousTrees.TreeSizes.size(), results);
    }
    /**
     * Evaluate raw formula prediction for one object. Uses all model trees
     * @param floatFeatures
     * @param catFeatures
     * @param result indexation is [classId]
     */
    void Calc(TConstArrayRef<float> floatFeatures,
              TConstArrayRef<int> catFeatures,
              TArrayRef<double> result) const {
        TVector<TConstArrayRef<float>> floatFeaturesVec = {floatFeatures};
        TVector<TConstArrayRef<int>> catFeaturesVec = {catFeatures};
        Calc(floatFeaturesVec, catFeaturesVec, result);
    }
    /**
     * Evaluate raw fomula predictions for objects. Uses model trees for interval [treeStart, treeEnd)
     * @param floatFeatures
     * @param catFeatures vector of vector of TStringBuf with categorical features strings
     * @param treeStart
     * @param treeEnd
     * @param results indexation is [objectIndex * ApproxDimension + classId]
     */
    void Calc(const TVector<TConstArrayRef<float>>& floatFeatures,
              const TVector<TVector<TStringBuf>>& catFeatures,
              size_t treeStart,
              size_t treeEnd,
              TArrayRef<double> results) const;
    /**
     * Evaluate raw fomula predictions for objects. Uses all model trees.
     * @param floatFeatures
     * @param catFeatures vector of vector of TStringBuf with categorical features strings
     * @param results indexation is [objectIndex * ApproxDimension + classId]
     */
    void Calc(const TVector<TConstArrayRef<float>>& floatFeatures,
              const TVector<TVector<TStringBuf>>& catFeatures,
              TArrayRef<double> results) const {
        Calc(floatFeatures, catFeatures, 0, ObliviousTrees.TreeSizes.size(), results);
    }
    /**
     * Truncate model to contain only trees from [begin; end) interval.
     * @param begin
     * @param end
     * @return model copy that contains only needed trees
     */
    TFullModel CopyTreeRange(size_t begin, size_t end) const {
        TFullModel result = *this;
        result.ObliviousTrees.Truncate(begin, end);
        return result;
    }

    /**
     * Internal usage only.
     * Updates indexes in CTR provider and recalculates metadata in Oblivious trees after model modifications.
     */
    void UpdateDynamicData() {
        ObliviousTrees.UpdateMetadata();
        if (CtrProvider) {
            CtrProvider->SetupBinFeatureIndexes(
                ObliviousTrees.FloatFeatures,
                ObliviousTrees.OneHotFeatures,
                ObliviousTrees.CatFeatures);
        }
    }
};

void OutputModel(const TFullModel& model, const TString& modelFile);
TFullModel ReadModel(const TString& modelFile);

enum class EModelExportType {
    CatboostBinary,
    AppleCoreML
};
/**
 * Export model in our binary or protobuf CoreML format
 * @param model
 * @param modelFile
 * @param format
 * @param userParametersJSON
 */
void ExportModel(const TFullModel& model, const TString& modelFile, const EModelExportType format = EModelExportType::CatboostBinary, const TString& userParametersJSON = "");

/**
 * Serialize model to string
 * @param model
 * @return
 */
TString SerializeModel(const TFullModel& model);
/**
 * Deserialize model from string
 * @param serializeModelString
 * @return
 */
TFullModel DeserializeModel(const TString& serializeModelString);
