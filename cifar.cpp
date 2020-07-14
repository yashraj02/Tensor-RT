/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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

//!
//! sampleDynamicReshape.cpp
//! This file contains the implementation of the dynamic reshape MNIST sample. It creates a network
//! using the MNIST ONNX model, and uses a second engine to resize inputs to the shape the model
//! expects.
//! It can be run with the following command:
//! Command: ./sample_dynamic_reshape [-h or --help [-d=/path/to/data/dir or --datadir=/path/to/data/dir]
//!

#include "BatchStream.h"
#include "EntropyCalibrator.h"
#include "argsParser.h"
#include "buffers.h"
#include "common.h"
#include "logger.h"
#include "parserOnnxConfig.h"

#include "NvInfer.h"
#include <cuda_runtime_api.h>
#include <random>

const std::string gSampleName = "TensorRT.sample_cifar";

//! \brief The SampleDynamicReshape class implementes the dynamic reshape sample.
//!
//! \details This class builds one engine that resizes a given input to the correct size, and a
//! second engine based on an ONNX MNIST model that generates a prediction.
//!
class SampleDynamicReshape
{
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

public:
    SampleDynamicReshape(const samplesCommon::OnnxSampleParams& params)
        : mParams(params)
    {
    }

    //!
    //! \brief Builds both engines.
    //!
    bool build();

    //!
    //! \brief Prepares the model for inference by creating execution contexts and allocating buffers.
    //!
    bool prepare();

    //!
    //! \brief Runs inference using TensorRT on a random image.
    //!
    bool infer();

private:
    bool buildPreprocessorEngine(const SampleUniquePtr<nvinfer1::IBuilder>& builder);
    bool buildPredictionEngine(const SampleUniquePtr<nvinfer1::IBuilder>& builder);

    Dims loadPGMFile(const std::string& fileName);
    bool validateOutput(int digit);

    samplesCommon::OnnxSampleParams mParams; //!< The parameters for the sample.

    nvinfer1::Dims mPredictionInputDims;  //!< The dimensions of the input of the MNIST model.
    nvinfer1::Dims mPredictionOutputDims; //!< The dimensions of the output of the MNIST model.

    // Engines used for inference. The first is used for resizing inputs, the second for prediction.
    SampleUniquePtr<nvinfer1::ICudaEngine> mPreprocessorEngine{nullptr}, mPredictionEngine{nullptr};

    SampleUniquePtr<nvinfer1::IExecutionContext> mPreprocessorContext{nullptr}, mPredictionContext{nullptr};

    samplesCommon::ManagedBuffer mInput{};          //!< Host and device buffers for the input.
    samplesCommon::DeviceBuffer mPredictionInput{}; //!< Device buffer for the output of the preprocessor, i.e. the
                                                    //!< input to the prediction model.
    samplesCommon::ManagedBuffer mOutput{};         //!< Host buffer for the ouptut

    template <typename T>
    SampleUniquePtr<T> makeUnique(T* t)
    {
        return SampleUniquePtr<T>{t};
    }
};

//!
//! \brief Builds the two engines required for inference.
//!
//! \details This function creates one TensorRT engine for resizing inputs to the correct sizes,
//!          then creates a TensorRT network by parsing the ONNX model and builds
//!          an engine that will be used to run inference (mPredictionEngine).
//!
//! \return Ruturns false if error in build preprocessor or predict engine.
//!
bool SampleDynamicReshape::build()
{
    auto builder = makeUnique(nvinfer1::createInferBuilder(sample::gLogger.getTRTLogger()));
    if (!builder)
    {
        sample::gLogError << "Create inference builder failed1." << std::endl;
        return false;
    }
    // This function will also set mPredictionInputDims and mPredictionOutputDims,
    // so it needs to be called before building the preprocessor.
	sample::gLogError << "Before calling preprcessing method 1" << std::endl;
    return buildPredictionEngine(builder) && buildPreprocessorEngine(builder);
}

//!
//! \brief Builds an engine for preprocessing (mPreprocessorEngine).
//!
//! \return Ruturns false if error in build preprocessor engine.
//!
bool SampleDynamicReshape::buildPreprocessorEngine(const SampleUniquePtr<nvinfer1::IBuilder>& builder)
{
    // Create the preprocessor engine using a network that supports full dimensions (createNetworkV2).
    auto preprocessorNetwork = makeUnique(
        builder->createNetworkV2(1U << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH)));
    if (!preprocessorNetwork)
    {
        sample::gLogError << "Create network failed2." << std::endl;
        return false;
    }

    // Reshape a dynamically shaped input to the size expected by the model, (1, 1, 32, 32).
    auto input = preprocessorNetwork->addInput("input", nvinfer1::DataType::kFLOAT, Dims4{-1, 1, -1, -1});
    auto resizeLayer = preprocessorNetwork->addResize(*input);
    resizeLayer->setOutputDimensions(mPredictionInputDims);
    preprocessorNetwork->markOutput(*resizeLayer->getOutput(0));

    // Finally, configure and build the preprocessor engine.
    auto preprocessorConfig = makeUnique(builder->createBuilderConfig());
    if (!preprocessorConfig)
    {
        sample::gLogError << "Create builder config failed3." << std::endl;
        return false;
    }
	sample::gLogError << "Approaching in method" << std::endl;
    // Create an optimization profile so that we can specify a range of input dimensions.
    auto profile = builder->createOptimizationProfile();
    // This profile will be valid for all images whose size falls in the range of [(1, 1, 1, 1), (1, 1, 64, 64)]
    // but TensorRT will optimize for (1, 1, 32, 32)
    // We do not need to check the return of setDimension and addOptimizationProfile here as all dims are explicitly set
    profile->setDimensions(input->getName(), OptProfileSelector::kMIN, Dims4{1, 1, 1, 1});
	sample::gLogError << "Passed min" << std::endl;
    profile->setDimensions(input->getName(), OptProfileSelector::kOPT, Dims4{1, 1, 32, 32});
	sample::gLogError << "Passed mid" << std::endl;
    profile->setDimensions(input->getName(), OptProfileSelector::kMAX, Dims4{1, 1, 64, 64});
	sample::gLogError << "Passed max" << std::endl;
    preprocessorConfig->addOptimizationProfile(profile);
	

    // Create a calibration profile.
    auto profileCalib = builder->createOptimizationProfile();
    const int calibBatchSize{5};
    // We do not need to check the return of setDimension and setCalibrationProfile here as all dims are explicitly set
    profileCalib->setDimensions(input->getName(), OptProfileSelector::kMIN, Dims4{1, 1, 32, 32});
    profileCalib->setDimensions(input->getName(), OptProfileSelector::kOPT, Dims4{16, 1, 32, 32});
    profileCalib->setDimensions(input->getName(), OptProfileSelector::kMAX, Dims4{32, 1, 32, 32});
    preprocessorConfig->setCalibrationProfile(profileCalib);

    std::unique_ptr<IInt8Calibrator> calibrator;
    if (mParams.int8)
    {	
		sample::gLogError << "Entered 1" << std::endl;
        preprocessorConfig->setFlag(BuilderFlag::kINT8);
        const int nCalibBatches{16};
        MNISTBatchStream calibrationStream(
            calibBatchSize, nCalibBatches, "images", "labels", mParams.dataDirs);
        calibrator.reset(
            new Int8EntropyCalibrator2<MNISTBatchStream>(calibrationStream, 0, "MNISTPreprocessor", "input"));
        preprocessorConfig->setInt8Calibrator(calibrator.get());
		sample::gLogError << "Passed 1" << std::endl;
    }

    mPreprocessorEngine = makeUnique(builder->buildEngineWithConfig(*preprocessorNetwork, *preprocessorConfig));
    if (!mPreprocessorEngine)
    {
        sample::gLogError << "Preprocessor engine build failed4." << std::endl;
        return false;
    }
    sample::gLogInfo << "Profile dimensions in preprocessor engine:" << std::endl;
    sample::gLogInfo << "    Minimum = " << mPreprocessorEngine->getProfileDimensions(0, 0, OptProfileSelector::kMIN)
                     << std::endl;
    sample::gLogInfo << "    Optimum = " << mPreprocessorEngine->getProfileDimensions(0, 0, OptProfileSelector::kOPT)
                     << std::endl;
    sample::gLogInfo << "    Maximum = " << mPreprocessorEngine->getProfileDimensions(0, 0, OptProfileSelector::kMAX)
                     << std::endl;
    return true;
}

//!
//! \brief Builds an engine for prediction (mPredictionEngine).
//!
//! \details This function builds an engine for the MNIST model, and updates mPredictionInputDims and
//! mPredictionOutputDims according to the dimensions specified by the model. The preprocessor reshapes inputs to
//! mPredictionInputDims.
//!
//! \return Ruturns false if error in build prediction engine.
//!
bool SampleDynamicReshape::buildPredictionEngine(const SampleUniquePtr<nvinfer1::IBuilder>& builder)
{
    // Create a network using the parser.
	sample::gLogError << "Inside this method 2" << std::endl;
	const auto explicitBatch = 1U << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    auto network = makeUnique(builder->createNetworkV2(explicitBatch));
    if (!network)
    {
        sample::gLogError << "Create network failed5." << std::endl;
        return false;
    }

    auto parser = samplesCommon::infer_object(nvonnxparser::createParser(*network, sample::gLogger.getTRTLogger()));
    bool parsingSuccess = parser->parseFromFile(locateFile(mParams.onnxFileName, mParams.dataDirs).c_str(),
        static_cast<int>(sample::gLogger.getReportableSeverity()));
    if (!parsingSuccess)
    {
        sample::gLogError << "Failed6 to parse model." << std::endl;
        return false;
    }

    // Attach a softmax layer to the end of the network.
    auto softmax = network->addSoftMax(*network->getOutput(0));
    // Set softmax axis to 1 since network output has shape [1, 10] in full dims mode
    softmax->setAxes(1 << 1);
    network->unmarkOutput(*network->getOutput(0));
    network->markOutput(*softmax->getOutput(0));

    // Get information about the inputs/outputs directly from the model.
    mPredictionInputDims = network->getInput(0)->getDimensions();
    mPredictionOutputDims = network->getOutput(0)->getDimensions();

    // Create a builder config
    auto config = makeUnique(builder->createBuilderConfig());
    if (!config)
    {
        sample::gLogError << "Create builder config failed7." << std::endl;
        return false;
    }
    config->setMaxWorkspaceSize(16_MiB);
    if (mParams.fp16)
    {
        config->setFlag(BuilderFlag::kFP16);
    }

    auto profileCalib = builder->createOptimizationProfile();
    const auto inputName = mParams.inputTensorNames[0].c_str();
    const int calibBatchSize{5};
    // We do not need to check the return of setDimension and setCalibrationProfile here as all dims are explicitly set
    profileCalib->setDimensions(inputName, OptProfileSelector::kMIN, Dims4{1, 1, 32, 32});
    profileCalib->setDimensions(inputName, OptProfileSelector::kOPT, Dims4{16, 1, 32, 32});
    profileCalib->setDimensions(inputName, OptProfileSelector::kMAX, Dims4{32, 1, 32, 32});
    config->setCalibrationProfile(profileCalib);

    std::unique_ptr<IInt8Calibrator> calibrator;
	sample::gLogError << mParams.int8 << std::endl;
    if (mParams.int8)
    {
		sample::gLogError << "Entered 2" << std::endl;
		sample::gLogError << "Inside config" << std::endl;
		config->setFlag(BuilderFlag::kINT8);
		sample::gLogError << "1" << std::endl;
        int nCalibBatches{16};
		sample::gLogError << "2" << std::endl;
        MNISTBatchStream calibrationStream(
            calibBatchSize, nCalibBatches, "images", "labels", mParams.dataDirs);
		sample::gLogError << "3" << std::endl;	
        calibrator.reset(
            new Int8EntropyCalibrator2<MNISTBatchStream>(calibrationStream, 0, "MNISTPrediction", inputName));
        config->setInt8Calibrator(calibrator.get());
		sample::gLogError << "Entered 2" << std::endl;
    }
    // Build the prediciton engine.
    mPredictionEngine = makeUnique(builder->buildEngineWithConfig(*network, *config));
    if (!mPredictionEngine)
    {
        sample::gLogError << "Prediction engine build failed8." << std::endl;
        return false;
    }
    return true;
}

//!
//! \brief Prepares the model for inference by creating an execution context and allocating buffers.
//!
//! \details This function sets up the sample for inference. This involves allocating buffers for the inputs and
//! outputs, as well as creating TensorRT execution contexts for both engines. This only needs to be called a single
//! time.
//!
//! \return Ruturns false if error in build preprocessor or predict context.
//!
bool SampleDynamicReshape::prepare()
{
    mPreprocessorContext = makeUnique(mPreprocessorEngine->createExecutionContext());
    if (!mPreprocessorContext)
    {
        sample::gLogError << "Preprocessor context build failed10." << std::endl;
        return false;
    }

    mPredictionContext = makeUnique(mPredictionEngine->createExecutionContext());
    if (!mPredictionContext)
    {
        sample::gLogError << "Prediction context build failed11." << std::endl;
        return false;
    }

    // Since input dimensions are not known ahead of time, we only allocate the output buffer and preprocessor output
    // buffer.
    mPredictionInput.resize(mPredictionInputDims);
    mOutput.hostBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    mOutput.deviceBuffer.resize(mPredictionOutputDims);
    return true;
}

//!
//! \brief Runs inference for this sample
//!
//! \details This function is the main execution function of the sample.
//! It runs inference for using a random image from the MNIST dataset as an input.
//!
bool SampleDynamicReshape::infer()
{
    // Load a random PGM file into a host buffer, then copy to device.
    std::random_device rd{};
    std::default_random_engine generator{rd()};
    std::uniform_int_distribution<int> digitDistribution{0, 9};
    int digit = 10;

    Dims inputDims = loadPGMFile(locateFile(std::to_string(digit) + ".pgm", mParams.dataDirs));
    mInput.deviceBuffer.resize(inputDims);
    CHECK(cudaMemcpy(
        mInput.deviceBuffer.data(), mInput.hostBuffer.data(), mInput.hostBuffer.nbBytes(), cudaMemcpyHostToDevice));

    // Set the input size for the preprocessor
    CHECK_RETURN_W_MSG(mPreprocessorContext->setBindingDimensions(0, inputDims), false, "Invalid binding dimensions.");

    // We can only run inference once all dynamic input shapes have been specified.
    if (!mPreprocessorContext->allInputDimensionsSpecified())
    {
        return false;
    }

    // Run the preprocessor to resize the input to the correct shape
    std::vector<void*> preprocessorBindings = {mInput.deviceBuffer.data(), mPredictionInput.data()};
    // For engines using full dims, we can use executeV2, which does not include a separate batch size parameter.
    bool status = mPreprocessorContext->executeV2(preprocessorBindings.data());
    if (!status)
    {
        return false;
    }

    // Next, run the model to generate a prediction.
    std::vector<void*> predicitonBindings = {mPredictionInput.data(), mOutput.deviceBuffer.data()};
    status = mPredictionContext->executeV2(predicitonBindings.data());
    if (!status)
    {
        return false;
    }

    // Copy the outputs back to the host and verify the output.
    CHECK(cudaMemcpy(mOutput.hostBuffer.data(), mOutput.deviceBuffer.data(), mOutput.deviceBuffer.nbBytes(),
        cudaMemcpyDeviceToHost));
    return validateOutput(digit);
}

//!
//! \brief Loads a PGM file into mInput and returns the dimensions of the loaded image.
//!
//! \details This function loads the specified PGM file into the input host buffer.
//!
Dims SampleDynamicReshape::loadPGMFile(const std::string& fileName)
{
    std::ifstream infile(fileName, std::ifstream::binary);
    assert(infile.is_open() && "Attempting to read from a file that is not open.");

    std::string magic;
    int h, w, max;
    infile >> magic >> h >> w >> max;

    infile.seekg(1, infile.cur);
    Dims4 inputDims{1, 1, h, w};
    size_t vol = samplesCommon::volume(inputDims);
    std::vector<uint8_t> fileData(vol);
    infile.read(reinterpret_cast<char*>(fileData.data()), vol);

    // Print an ascii representation
    sample::gLogInfo << "Input:\n";
    for (size_t i = 0; i < vol; i++)
    {
        sample::gLogInfo << (" .:-=+*#%@"[fileData[i] / 26]) << (((i + 1) % w) ? "" : "\n");
    }
    sample::gLogInfo << std::endl;

    // Normalize and copy to the host buffer.
    mInput.hostBuffer.resize(inputDims);
    float* hostDataBuffer = static_cast<float*>(mInput.hostBuffer.data());
    std::transform(fileData.begin(), fileData.end(), hostDataBuffer,
        [](uint8_t x) { return 1.0 - static_cast<float>(x / 255.0); });
    return inputDims;
}

//!
//! \brief Checks whether the model prediction (in mOutput) is correct.
//!
bool SampleDynamicReshape::validateOutput(int digit)
{
    const float* bufRaw = static_cast<const float*>(mOutput.hostBuffer.data());
    std::vector<float> prob(bufRaw, bufRaw + mOutput.hostBuffer.size());

    int curIndex{0};
    for (const auto& elem : prob)
    {
        sample::gLogInfo << " Prob " << curIndex << "  " << std::fixed << std::setw(5) << std::setprecision(4) << elem
                         << " "
                         << "Class " << curIndex << ": " << std::string(int(std::floor(elem * 10 + 0.5f)), '*')
                         << std::endl;
        ++curIndex;
    }

    int predictedDigit = std::max_element(prob.begin(), prob.end()) - prob.begin();
    return digit == predictedDigit;
}

//!
//! \brief Initializes members of the params struct using the command line args
//!
samplesCommon::OnnxSampleParams initializeSampleParams(const samplesCommon::Args& args)
{
    samplesCommon::OnnxSampleParams params;
    if (args.dataDirs.empty()) //!< Use default directories if user hasn't provided directory paths
    {
        params.dataDirs.push_back("data/cifar/");
        params.dataDirs.push_back("data/samples/cifar/");
    }
    else //!< Use the data directory provided by the user
    {
        params.dataDirs = args.dataDirs;
    }
    params.onnxFileName = "cifar.onnx";
    params.inputTensorNames.push_back("conv2d_input:0");
    params.outputTensorNames.push_back("Identity:0");
    params.int8 = args.runInInt8;
    params.fp16 = args.runInFp16;
    return params;
}

//!
//! \brief Prints the help information for running this sample
//!
void printHelpInfo()
{
    std::cout << "Usage: ./cifar [-h or --help] [-d or --datadir=<path to data directory>]"
              << std::endl;
    std::cout << "--help, -h      Display help information" << std::endl;
    std::cout << "--datadir       Specify path to a data directory, overriding the default. This option can be used "
                 "multiple times to add multiple directories. If no data directories are given, the default is to use "
                 "(data/samples/cifar/, data/cifar/)"
              << std::endl;
    std::cout << "--int8          Run in Int8 mode." << std::endl;
    std::cout << "--fp16          Run in FP16 mode." << std::endl;
}

int main(int argc, char** argv)
{
    samplesCommon::Args args;
    bool argsOK = samplesCommon::parseArgs(args, argc, argv);
    if (!argsOK)
    {
        sample::gLogError << "Invalid arguments" << std::endl;
        printHelpInfo();
        return EXIT_FAILURE;
    }
    if (args.help)
    {
        printHelpInfo();
        return EXIT_SUCCESS;
    }

    auto sampleTest = sample::gLogger.defineTest(gSampleName, argc, argv);

    sample::gLogger.reportTestStart(sampleTest);

    SampleDynamicReshape sample{initializeSampleParams(args)};

    if (!sample.build())
    {
        return sample::gLogger.reportFail(sampleTest);
    }
    if (!sample.prepare())
    {
        return sample::gLogger.reportFail(sampleTest);
    }
    if (!sample.infer())
    {
        return sample::gLogger.reportFail(sampleTest);
    }

    return sample::gLogger.reportPass(sampleTest);
}