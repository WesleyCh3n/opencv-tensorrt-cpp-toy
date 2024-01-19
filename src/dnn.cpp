#include "dnn.hpp"

void dnn::letterbox(const cv::cuda::GpuMat &input,
                    cv::cuda::GpuMat &output_image, const cv::Size &new_size,
                    const cv::Size &target_size) {
  cv::cuda::resize(input, output_image, new_size, 0, 0, cv::INTER_AREA);
  float padh = (target_size.height - new_size.height) / 2.;
  float padw = (target_size.width - new_size.width) / 2.;
  int top = std::round(padh - 0.1);
  int bottom = std::round(padh + 0.1);
  int left = std::round(padw - 0.1);
  int right = std::round(padw + 0.1);
  cv::cuda::copyMakeBorder(output_image, output_image, top, bottom, left, right,
                           cv::BORDER_CONSTANT, cv::Scalar(114.));
}

cv::cuda::GpuMat dnn::blob_from_gpumat(const cv::cuda::GpuMat &input,
                                       const std::array<float, 3> &std,
                                       const std::array<float, 3> &mean,
                                       bool swapBR, bool normalize) {
  if (swapBR)
    cv::cuda::cvtColor(input, input, cv::COLOR_BGR2RGB);
  cv::cuda::GpuMat blob(1, input.rows * input.cols, CV_8UC3);
  size_t continuous_length = input.rows * input.cols;
  std::vector<cv::cuda::GpuMat> rgb{
      cv::cuda::GpuMat(input.rows, input.cols, CV_8U, &(blob.ptr()[0])),
      cv::cuda::GpuMat(input.rows, input.cols, CV_8U,
                       &(blob.ptr()[continuous_length])),
      cv::cuda::GpuMat(input.rows, input.cols, CV_8U,
                       &(blob.ptr()[continuous_length * 2])),
  };
  cv::cuda::split(input, rgb);
  if (normalize) {
    blob.convertTo(blob, CV_32FC3, 1.f / 255.f);
  } else {
    blob.convertTo(blob, CV_32FC3);
  }
  cv::cuda::subtract(blob, cv::Scalar(mean[0], mean[1], mean[2]), blob,
                     cv::noArray(), -1);
  cv::cuda::divide(blob, cv::Scalar(std[0], std[1], std[2]), blob, 1, -1);
  return blob;
}

cv::cuda::GpuMat
dnn::blob_from_gpumat(const std::vector<cv::cuda::GpuMat> &inputs,
                      const std::array<float, 3> &std,
                      const std::array<float, 3> &mean, bool swapBR,
                      bool normalize) {
  if (swapBR) {
    for (uint32_t i = 0; i < (uint32_t)inputs.size(); i++) {
      cv::cuda::cvtColor(inputs[i], inputs[i], cv::COLOR_BGR2RGB);
    }
  }

  cv::cuda::GpuMat blob(1, inputs[0].rows * inputs[0].cols * inputs.size(),
                        CV_8UC3);
  size_t continuous_length = inputs[0].rows * inputs[0].cols;
  for (uint32_t i = 0; i < (uint32_t)inputs.size(); i++) {
    std::vector<cv::cuda::GpuMat> rgb{
        cv::cuda::GpuMat(inputs[0].rows, inputs[0].cols, CV_8U,
                         &(blob.ptr()[0 + continuous_length * 3 * i])),
        cv::cuda::GpuMat(
            inputs[0].rows, inputs[0].cols, CV_8U,
            &(blob.ptr()[continuous_length + continuous_length * 3 * i])),
        cv::cuda::GpuMat(
            inputs[0].rows, inputs[0].cols, CV_8U,
            &(blob.ptr()[continuous_length * 2 + continuous_length * 3 * i])),
    };
    cv::cuda::split(inputs[i], rgb);
  }
  if (normalize) {
    blob.convertTo(blob, CV_32FC3, 1.f / 255.f);
  } else {
    blob.convertTo(blob, CV_32FC3);
  }
  cv::cuda::subtract(blob, cv::Scalar(mean[0], mean[1], mean[2]), blob,
                     cv::noArray(), -1);
  cv::cuda::divide(blob, cv::Scalar(std[0], std[1], std[2]), blob, 1, -1);
  return blob;
}

dnn::Yolo::Yolo(std::filesystem::path model_path,
                const uint32_t max_batch_size) {
  auto option = trt::EngineOption{max_batch_size};
  model_ = std::make_unique<trt::Engine>(model_path.string(), option);
  input_dim_ = model_->get_input_dims();
  output_dim_ = model_->get_output_dims();
}

std::vector<dnn::Object> dnn::Yolo::predict(const cv::cuda::GpuMat &gmat,
                                            const float &confidence_threshold,
                                            const float &nms_threshold) {
  cv::cuda::GpuMat input;
  int img_cols = gmat.cols;
  int img_rows = gmat.rows;
  float scale = std::min((float)input_dim_[2] / img_cols,
                         (float)input_dim_[1] / img_rows);
  const auto padding = cv::Size{
      static_cast<int>(std::round(input_dim_[2] - img_cols * scale) / 2 - 0.1),
      static_cast<int>(std::round(input_dim_[1] - img_rows * scale) / 2 - 0.1)};
  letterbox(gmat, input,
            {static_cast<int>(std::round(img_cols * scale)),
             static_cast<int>(std::round(img_rows * scale))},
            {int(input_dim_[2]), int(input_dim_[1])});
  auto blob = blob_from_gpumat(input,                         // input gpumats
                               std::array<float, 3>{1, 1, 1}, // std factor
                               std::array<float, 3>{0, 0, 0}, // mean
                               true, true);

  model_->run(blob, 1, raw_output_);
  auto results =
      post_process(raw_output_.data(), confidence_threshold, nms_threshold,
                   scale, padding, img_cols, img_rows);
  return results;
}

std::vector<std::vector<dnn::Object>>
dnn::Yolo::predict(const std::vector<cv::cuda::GpuMat> &gmats,
                   const float &confidence_threshold,
                   const float &nms_threshold) {
  uint32_t batch_size = gmats.size();
  std::vector<cv::cuda::GpuMat> inputs(batch_size);
  int img_cols = gmats[0].cols;
  int img_rows = gmats[0].rows;
  int in_cols = input_dim_[2];
  int in_rows = input_dim_[1];
  float scale = std::min((float)in_cols / img_cols, (float)in_rows / img_rows);
  const auto padding = cv::Size{
      static_cast<int>(std::round(in_cols - img_cols * scale) / 2 - 0.1),
      static_cast<int>(std::round(in_rows - img_rows * scale) / 2 - 0.1)};
  for (int i = 0; i < gmats.size(); i++) {
    letterbox(gmats[i], inputs[i],
              {static_cast<int>(std::round(img_cols * scale)),
               static_cast<int>(std::round(img_rows * scale))},
              {in_cols, in_rows});
  }
  auto blob = blob_from_gpumat(inputs,                        // input gpumats
                               std::array<float, 3>{1, 1, 1}, // std factor
                               std::array<float, 3>{0, 0, 0}, // mean
                               true, true);
  model_->run(blob, batch_size, raw_output_);
  auto results =
      post_process(raw_output_.data(), inputs.size(), confidence_threshold,
                   nms_threshold, scale, padding, img_cols, img_rows);
  return results;
}

std::vector<dnn::Object>
dnn::Yolo::post_process(float *raw_results, const float &confidence_threshold_,
                        const float &nms_threshold_, const float &scale,
                        const cv::Size &pad, const uint32_t img_cols,
                        const uint32_t img_rows) {
  if (output_dim_[0] >= 6) {
    throw std::runtime_error("xywhsc is not supported yet");
  }
  std::vector<cv::Rect> rects;
  std::vector<float> confs;
  std::vector<int> idxes;
  for (int i = 0; i < output_dim_[1]; i++) {
    if (raw_results[4 * output_dim_[1] + i] > confidence_threshold_) {
      float xc = raw_results[i];
      float yc = raw_results[1 * output_dim_[1] + i];
      float dw = raw_results[2 * output_dim_[1] + i] / 2;
      float dh = raw_results[3 * output_dim_[1] + i] / 2;
      float x1 = xc - dw;
      float y1 = yc - dh;
      float x2 = xc + dw;
      float y2 = yc + dh;

      x1 = std::max((x1 - pad.width) / scale, (float)0.0);
      y1 = std::max((y1 - pad.height) / scale, (float)0.0);
      x2 = std::min((x2 - pad.width) / scale, (float)img_cols);
      y2 = std::min((y2 - pad.height) / scale, (float)img_rows);

      rects.emplace_back(cv::Rect(cv::Point(std::round(x1), std::round(y1)),
                                  cv::Point(std::round(x2), std::round(y2))));
      confs.emplace_back(raw_results[4 * output_dim_[1] + i]);
    }
  }
  if (!rects.empty()) {
    cv::dnn::NMSBoxes(rects, confs, confidence_threshold_, nms_threshold_,
                      idxes);
  }
  std::vector<Object> objs;
  for (auto &i : idxes) {
    objs.emplace_back(Object{rects[i], confs[i]});
  }
  return objs;
};

std::vector<std::vector<dnn::Object>>
dnn::Yolo::post_process(float *raw_results, const uint32_t batch_size,
                        const float &confidence_threshold_,
                        const float &nms_threshold_, const float &scale,
                        const cv::Size &pad, const uint32_t img_cols,
                        const uint32_t img_rows) {
  std::vector<std::vector<Object>> results(batch_size);
  for (int b = 0; b < batch_size; b++) {
    results[b] = post_process(raw_results + b * output_dim_[0] * output_dim_[1],
                              0.25, 0.45, scale, pad, img_cols, img_rows);
  }
  return results;
};

// =============================================================================
// FeatureExtractor
// =============================================================================

dnn::FeatureExtractor::FeatureExtractor(std::filesystem::path model_path,
                                        const uint32_t max_batch_size) {
  auto option = trt::EngineOption{max_batch_size};
  model_ = std::make_unique<trt::Engine>(model_path.string(), option);
  input_dim_ = model_->get_input_dims();
  output_dim_ = model_->get_output_dims();
}

std::vector<float>
dnn::FeatureExtractor::predict(const cv::cuda::GpuMat &gmat,
                               const std::array<float, 3> &std,
                               const std::array<float, 3> &mean) {
  cv::cuda::GpuMat resized;
  cv::cuda::resize(gmat, resized, cv::Size(input_dim_[1], input_dim_[2]), 0, 0,
                   cv::INTER_AREA);
  auto blob = blob_from_gpumat(resized, std, mean, true, true);
  model_->run(blob, 1, raw_output_);
  return std::move(raw_output_);
}

std::vector<float>
dnn::FeatureExtractor::predict(const std::vector<cv::cuda::GpuMat> &gmats,
                               const std::array<float, 3> &std,
                               const std::array<float, 3> &mean) {
  std::vector<cv::cuda::GpuMat> resized(gmats.size());
  for (int i = 0; i < gmats.size(); i++) {
    cv::cuda::resize(gmats[i], resized[i],
                     cv::Size(input_dim_[1], input_dim_[2]), 0, 0,
                     cv::INTER_AREA);
  }

  auto blob = blob_from_gpumat(resized, std, mean, true, true);

  model_->run(blob, gmats.size(), raw_output_);
  return std::move(raw_output_);
}
