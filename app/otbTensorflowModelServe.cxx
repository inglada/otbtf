/*=========================================================================

  Copyright (c) Remi Cresson (IRSTEA). All rights reserved.


     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#include "itkFixedArray.h"
#include "itkObjectFactory.h"
#include "otbWrapperApplicationFactory.h"

// Application engine
#include "otbStandardFilterWatcher.h"
#include "itkFixedArray.h"

// Tensorflow stuff
#include "tensorflow/core/public/session.h"
#include "tensorflow/core/platform/env.h"

// Tensorflow model filter
#include "otbTensorflowMultisourceModelFilter.h"

// Tensorflow graph load
#include "otbTensorflowGraphOperations.h"

// Layerstack
#include "otbTensorflowSource.h"

// Streaming
#include "otbImageRegionSquareTileSplitter.h"
#include "itkStreamingImageFilter.h"

namespace otb
{

namespace Wrapper
{

class TensorflowModelServe : public Application
{
public:
  /** Standard class typedefs. */
  typedef TensorflowModelServe                       Self;
  typedef Application                                Superclass;
  typedef itk::SmartPointer<Self>                    Pointer;
  typedef itk::SmartPointer<const Self>              ConstPointer;

  /** Standard macro */
  itkNewMacro(Self);
  itkTypeMacro(TensorflowModelServe, Application);

  /** Typedefs for tensorflow */
  typedef otb::TensorflowMultisourceModelFilter<FloatVectorImageType, FloatVectorImageType> TFModelFilterType;
  typedef otb::TensorflowSource<FloatVectorImageType> InputImageSource;

  /** Typedef for streaming */
  typedef otb::ImageRegionSquareTileSplitter<FloatVectorImageType::ImageDimension> TileSplitterType;
  typedef itk::StreamingImageFilter<FloatVectorImageType, FloatVectorImageType> StreamingFilterType;

  /** Typedefs for images */
  typedef FloatVectorImageType::SizeType SizeType;

  void DoUpdateParameters()
  {
  }

  //
  // Store stuff related to one source
  //
  struct ProcessObjectsBundle
  {
    InputImageSource m_ImageSource;
    SizeType         m_PatchSize;
    std::string      m_Placeholder;

    // Parameters keys
    std::string m_KeyIn;     // Key of input image list
    std::string m_KeyPszX;   // Key for samples sizes X
    std::string m_KeyPszY;   // Key for samples sizes Y
    std::string m_KeyPHName; // Key for placeholder name in the tensorflow model
  };

  //
  // Add an input source, which includes:
  // -an input image list
  // -an input patchsize (dimensions of samples)
  //
  void AddAnInputImage()
  {
    // Number of source
    unsigned int inputNumber = m_Bundles.size() + 1;

    // Create keys and descriptions
    std::stringstream ss_key_group, ss_desc_group,
    ss_key_in, ss_desc_in,
    ss_key_dims_x, ss_desc_dims_x,
    ss_key_dims_y, ss_desc_dims_y,
    ss_key_ph, ss_desc_ph;

    // Parameter group key/description
    ss_key_group  << "source"                  << inputNumber;
    ss_desc_group << "Parameters for source #" << inputNumber;

    // Parameter group keys
    ss_key_in      << ss_key_group.str() << ".il";
    ss_key_dims_x  << ss_key_group.str() << ".fovx";
    ss_key_dims_y  << ss_key_group.str() << ".fovy";
    ss_key_ph      << ss_key_group.str() << ".placeholder";

    // Parameter group descriptions
    ss_desc_in     << "Input image (or list to stack) for source #" << inputNumber;
    ss_desc_dims_x << "Field of view width for source #"            << inputNumber;
    ss_desc_dims_y << "Field of view height for source #"           << inputNumber;
    ss_desc_ph     << "Name of the input placeholder for source #"  << inputNumber;

    // Populate group
    AddParameter(ParameterType_Group,          ss_key_group.str(),  ss_desc_group.str());
    AddParameter(ParameterType_InputImageList, ss_key_in.str(),     ss_desc_in.str() );
    AddParameter(ParameterType_Int,            ss_key_dims_x.str(), ss_desc_dims_x.str());
    SetMinimumParameterIntValue               (ss_key_dims_x.str(), 1);
    AddParameter(ParameterType_Int,            ss_key_dims_y.str(), ss_desc_dims_y.str());
    SetMinimumParameterIntValue               (ss_key_dims_y.str(), 1);
    AddParameter(ParameterType_String,         ss_key_ph.str(),     ss_desc_ph.str());

    // Add a new bundle
    ProcessObjectsBundle bundle;
    bundle.m_KeyIn     = ss_key_in.str();
    bundle.m_KeyPszX   = ss_key_dims_x.str();
    bundle.m_KeyPszY   = ss_key_dims_y.str();
    bundle.m_KeyPHName = ss_key_ph.str();

    m_Bundles.push_back(bundle);

  }

  void DoInit()
  {

    // Documentation
    SetName("TensorflowModelServe");
    SetDescription("Multisource deep learning classifier using Tensorflow. Change "
        "the " + tf::ENV_VAR_NAME_NSOURCES + " environment variable to set the number of "
        "sources.");
    SetDocLongDescription("The application run a Tensorflow model over multiple data sources. "
        "The number of input sources can be changed at runtime by setting the "
        "system environment variable " + tf::ENV_VAR_NAME_NSOURCES + ". "
        "For each source, you have to set (1) the tensor placeholder name, as named in "
        "the tensorflow model, (2) the patch size and (3) the image(s) source. "
        "The output is a multiband image, stacking all outputs "
        "tensors together: you have to specify the names of the output tensors, as "
        "named in the tensorflow model (typically, an operator's output). The output "
        "tensors values will be stacked in the same order as they appear in the "
        "\"model.output\" parameter (you can use a space separator between names). "
        "Last but not least, consider using extended filename to bypass the automatic "
        "memory footprint calculator of the otb application engine, and set a good "
        "splitting strategy (I would recommend using small square tiles) or use the "
        "finetuning parameter group to impose your squared tiles sizes");
    SetDocAuthors("Remi Cresson");

    // Input/output images
    AddAnInputImage();
    for (int i = 1; i < tf::GetNumberOfSources() ; i++)
      AddAnInputImage();

    // Input model
    AddParameter(ParameterType_Group,         "model",           "model parameters");
    AddParameter(ParameterType_Directory,     "model.dir",       "Tensorflow model_save directory");
    MandatoryOn                              ("model.dir");
    AddParameter(ParameterType_StringList,    "model.userplaceholders",    "Additional single-valued placeholders. Supported types: int, float, bool.");
    MandatoryOff                             ("model.userplaceholders");
    AddParameter(ParameterType_Bool,          "model.fullyconv", "Fully convolutional");
    MandatoryOff                             ("model.fullyconv");

    // Output tensors parameters
    AddParameter(ParameterType_Group,         "output",          "Output tensors parameters");
    AddParameter(ParameterType_Float,         "output.spcscale", "The output spacing scale");
    SetDefaultParameterFloat                 ("output.spcscale", 1.0);
    AddParameter(ParameterType_StringList,    "output.names",    "Names of the output tensors");
    MandatoryOn                              ("output.names");

    // Output Field of Expression
    AddParameter(ParameterType_Int,           "output.foex", "The output field of expression (x)");
    SetMinimumParameterIntValue              ("output.foex", 1);
    SetDefaultParameterInt                   ("output.foex", 1);
    MandatoryOn                              ("output.foex");
    AddParameter(ParameterType_Int,           "output.foey", "The output field of expression (y)");
    SetMinimumParameterIntValue              ("output.foey", 1);
    SetDefaultParameterInt                   ("output.foey", 1);
    MandatoryOn                              ("output.foey");

    // Fine tuning
    AddParameter(ParameterType_Group,         "finetuning" , "Fine tuning performance or consistency parameters");
    AddParameter(ParameterType_Bool,          "finetuning.disabletiling", "Disable tiling");
    MandatoryOff                             ("finetuning.disabletiling");
    AddParameter(ParameterType_Int,           "finetuning.tilesize", "Tile width used to stream the filter output");
    SetMinimumParameterIntValue              ("finetuning.tilesize", 1);
    SetDefaultParameterInt                   ("finetuning.tilesize", 16);

    // Output image
    AddParameter(ParameterType_OutputImage, "out", "output image");

    // Example
    SetDocExampleParameterValue("source1.il",             "spot6pms.tif");
    SetDocExampleParameterValue("source1.placeholder",    "x1");
    SetDocExampleParameterValue("source1.fovx",           "16");
    SetDocExampleParameterValue("source1.fovy",           "16");
    SetDocExampleParameterValue("model.dir",              "/tmp/my_saved_model/");
    SetDocExampleParameterValue("model.userplaceholders", "is_training=false dropout=0.0");
    SetDocExampleParameterValue("output.names",           "out_predict1 out_proba1");
    SetDocExampleParameterValue("out",                    "\"classif128tgt.tif?&streaming:type=tiled&streaming:sizemode=height&streaming:sizevalue=256\"");

  }

  //
  // Prepare bundles from the number of points
  //
  void PrepareInputs()
  {

    for (auto& bundle: m_Bundles)
    {
      // Setting the image source
      FloatVectorImageListType::Pointer list = GetParameterImageList(bundle.m_KeyIn);
      bundle.m_ImageSource.Set(list);
      bundle.m_Placeholder = GetParameterAsString(bundle.m_KeyPHName);
      bundle.m_PatchSize[0] = GetParameterInt(bundle.m_KeyPszX);
      bundle.m_PatchSize[1] = GetParameterInt(bundle.m_KeyPszY);

      otbAppLogINFO("Source info :");
      otbAppLogINFO("Field of view : " << bundle.m_PatchSize  );
      otbAppLogINFO("Placeholder   : " << bundle.m_Placeholder);
    }
  }

  void DoExecute()
  {

    // Load the Tensorflow bundle
    tf::LoadModel(GetParameterAsString("model.dir"), m_SavedModel);

    // Prepare inputs
    PrepareInputs();

    // Setup filter
    m_TFFilter = TFModelFilterType::New();
    m_TFFilter->SetGraph(m_SavedModel.meta_graph_def.graph_def());
    m_TFFilter->SetSession(m_SavedModel.session.get());
    m_TFFilter->SetOutputTensorsNames(GetParameterStringList("output.names"));
    m_TFFilter->SetOutputSpacingScale(GetParameterFloat("output.spcscale"));
    otbAppLogINFO("Output spacing ratio: " << m_TFFilter->GetOutputSpacingScale());

    // Get user placeholders
    TFModelFilterType::DictListType dict;
    TFModelFilterType::StringList expressions = GetParameterStringList("model.userplaceholders");
    for (auto& exp: expressions)
    {
      TFModelFilterType::DictType entry = tf::ExpressionToTensor(exp);
      dict.push_back(entry);

      otbAppLogINFO("Using placeholder " << entry.first << " with " << tf::PrintTensorInfos(entry.second));
    }
    m_TFFilter->SetUserPlaceholders(dict);

    // Input sources
    for (auto& bundle: m_Bundles)
    {
      m_TFFilter->PushBackInputBundle(bundle.m_Placeholder, bundle.m_PatchSize, bundle.m_ImageSource.Get());
    }

    // Fully convolutional mode on/off
    if (GetParameterInt("model.fullyconv")==1)
    {
      otbAppLogINFO("The tensorflow model is used in fully convolutional mode");
      m_TFFilter->SetFullyConvolutional(true);
    }

    // Output field of expression
    FloatVectorImageType::SizeType foe;
    foe[0] = GetParameterInt("output.foex");
    foe[1] = GetParameterInt("output.foey");
    m_TFFilter->SetOutputFOESize(foe);

    otbAppLogINFO("Output field of expression: " << m_TFFilter->GetOutputFOESize());

    // Streaming
    if (GetParameterInt("finetuning.disabletiling")!=1)
    {
      // Get the tile size
      const unsigned int tileSize = GetParameterInt("finetuning.tilesize");
      otbAppLogINFO("Force tiling with squared tiles of " << tileSize)

      // Update the TF filter to get the output image size
      m_TFFilter->UpdateOutputInformation();

      // Splitting using square tiles
      TileSplitterType::Pointer splitter = TileSplitterType::New();
      splitter->SetTileSizeAlignment(tileSize);
      unsigned int nbDesiredTiles = itk::Math::Ceil<unsigned int>(
          double(m_TFFilter->GetOutput()->GetLargestPossibleRegion().GetNumberOfPixels() ) / (tileSize * tileSize) );

      // Use an itk::StreamingImageFilter to force the computation on tiles
      m_StreamFilter = StreamingFilterType::New();
      m_StreamFilter->SetRegionSplitter(splitter);
      m_StreamFilter->SetNumberOfStreamDivisions(nbDesiredTiles);
      m_StreamFilter->SetInput(m_TFFilter->GetOutput());

      SetParameterOutputImage("out", m_StreamFilter->GetOutput());
    }
    else
    {
      otbAppLogINFO("Tiling disabled");
      SetParameterOutputImage("out", m_TFFilter->GetOutput());

    }
  }

private:

  TFModelFilterType::Pointer   m_TFFilter;
  StreamingFilterType::Pointer m_StreamFilter;
  tensorflow::SavedModelBundle m_SavedModel; // must be alive during all the execution of the application !

  std::vector<ProcessObjectsBundle>           m_Bundles;

}; // end of class

} // namespace wrapper
} // namespace otb

OTB_APPLICATION_EXPORT( otb::Wrapper::TensorflowModelServe )
