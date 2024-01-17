#include <opencv2/opencv.hpp>

#include <cxxopts.hpp>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include "dnn.hpp"

namespace global {
std::string model_path;
std::string image_path;
uint32_t batch_size;
uint32_t max_batch_size;
} // namespace global

void parse_args(int argc, char *argv[]) {
  cxxopts::Options options("resnet_single",
                           "TensorRT Resnet50 single image example");
  // clang-format off
  options.add_options()
    ("model", "model path", cxxopts::value<std::string>())
    ("input", "single image path", cxxopts::value<std::string>())
    ("b,batch", "batch size", cxxopts::value<int>()->default_value("512"))
    ("m,maxbatch", "max batch size of model", cxxopts::value<int>()->default_value("512"))
    ("h,help", "help");
  options.parse_positional({"model", "input"});
  options.positional_help("<model path> <input img path>");
  options.show_positional_help();
  auto result = options.parse(argc, argv);
  // clang-format on
  try {
    if (result.count("help")) {
      std::cout << options.help();
      exit(0);
    }
    global::model_path = result["model"].as<std::string>().c_str();
    global::image_path = result["input"].as<std::string>().c_str();
    global::batch_size = result["batch"].as<int>();
    global::max_batch_size = result["maxbatch"].as<int>();
    assert(global::batch_size <= global::max_batch_size);
  } catch (const cxxopts::exceptions::exception &e) {
    spdlog::error(e.what());
    spdlog::info(options.help());
    exit(1);
  }
}

void process_single_img() {
  dnn::FeatureExtractor model(global::model_path, global::max_batch_size);

  auto cpumat = cv::imread(global::image_path);
  auto gpumat = cv::cuda::GpuMat(cpumat);

  auto r = model.predict({gpumat, gpumat});

  std::cout << r.size() << '\n';
  for (int b = 0; b < 2; b++) {
    for (int i = 0; i < 10; i++) {
      std::cout << r[b * 512 + i] << ' ';
    }
    std::cout << std::endl;
  }
}

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::trace);
  try {
    parse_args(argc, argv);
    trt::check_cuda_err(cudaSetDevice(0));
    process_single_img();
  } catch (trt::EngineException &e) {
    spdlog::error("trt::EngineException: {}", e.what());
    return 1;
  } catch (trt::CudaException &e) {
    spdlog::error("trt::CudaException: {}", e.what());
    return 1;
  } catch (std::exception &e) {
    spdlog::error("std::exception: {}", e.what());
    return 1;
  }
  return 0;
}
