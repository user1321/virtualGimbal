/**
* This file is part of VirtualGimbal.
*
* Copyright 2019 Yoshiaki Sato <virtualgimbal at xa2 dot so-net dot ne dot jp>
*
* VirtualGimbal is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.

* VirtualGimbal is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.

* You should have received a copy of the GNU General Public License
* along with VirtualGimbal.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "virtual_gimbal_manager.h"

using namespace cv;
using namespace std;

VirtualGimbalManager::VirtualGimbalManager()
{
}

std::string VirtualGimbalManager::getVideoSize(const char *videoName)
{
    std::shared_ptr<cv::VideoCapture> Capture = std::make_shared<cv::VideoCapture>(videoName); //動画をオープン
    assert(Capture->isOpened());
    std::string videoSize = std::to_string((int)Capture->get(cv::CAP_PROP_FRAME_WIDTH)) + std::string("x") + std::to_string((int)Capture->get(cv::CAP_PROP_FRAME_HEIGHT));
    return videoSize;
}

void VirtualGimbalManager::setVideoParam(const char *file_name, CameraInformationPtr info)
{
    std::shared_ptr<cv::VideoCapture> capture = std::make_shared<cv::VideoCapture>(file_name); //動画をオープン
    if (!capture->isOpened())
    {
        throw "Video not found.";
    }
    video_param.reset(new Video(capture->get(cv::CAP_PROP_FPS)));
    video_param->video_frames = capture->get(cv::CAP_PROP_FRAME_COUNT);
    video_param->rolling_shutter_time = 0.0;
    video_param->camera_info = info;
    video_param->video_file_name = file_name;
}

void VirtualGimbalManager::setMeasuredAngularVelocity(const char *file_name, CameraInformationPtr info)
{
    measured_angular_velocity.reset(new AngularVelocity(readSamplingRateFromJson(file_name)));
    measured_angular_velocity->data = readAngularVelocityFromJson(file_name);
    if (info)
    {
        rotateAngularVelocity(measured_angular_velocity->data, info->sd_card_rotation_);
    }
}

/**
 * @brief For angular velocity from optical flow 
 **/
// void VirtualGimbalManager::setEstimatedAngularVelocity(const char *file_name, CameraInformationPtr info, int32_t maximum_synchronize_frames)
// {
//     std::shared_ptr<cv::VideoCapture> capture = std::make_shared<cv::VideoCapture>(file_name); //動画をオープン

//     Eigen::MatrixXd optical_flow;

//     calcShiftFromVideo(capture, maximum_synchronize_frames, optical_flow);

//     estimated_angular_velocity.reset(new AngularVelocity(capture->get(cv::CAP_PROP_FPS)));
//     estimated_angular_velocity->data.resize(optical_flow.rows(), optical_flow.cols());

//     estimated_angular_velocity->data.col(0) =
//         optical_flow.col(1).unaryExpr([&](double a) { return estimated_angular_velocity->getFrequency() * atan(a / (-info->fy_)); });
//     estimated_angular_velocity->data.col(1) =
//         optical_flow.col(0).unaryExpr([&](double a) { return estimated_angular_velocity->getFrequency() * -atan(a / (info->fx_)); });
//     estimated_angular_velocity->data.col(2) = -estimated_angular_velocity->getFrequency() * optical_flow.col(2);
// }

/**
 * @brief For angular velocity from chess board 
 **/
void VirtualGimbalManager::setEstimatedAngularVelocity(Eigen::MatrixXd &angular_velocity, Eigen::VectorXd confidence, double frequency)
{
    if (0.0 == frequency)
    {
        estimated_angular_velocity = std::make_shared<AngularVelocity>(video_param->getFrequency());
    }
    else
    {
        estimated_angular_velocity = std::make_shared<AngularVelocity>(frequency);
    }
    estimated_angular_velocity->confidence = confidence;
    estimated_angular_velocity->data = angular_velocity;
}

void VirtualGimbalManager::setRotation(const char *file_name, CameraInformation &cameraInfo)
{
    rotation.reset(new Rotation());
    // std::vector<Eigen::Vector3d,Eigen::aligned_allocator<Eigen::Vector3d>> angular_velocity;
    // if(file_name){
    //     readAngularVelocityFromJson(angular_velocity,file_name);
    // }else{
    //     throw  "Json file not found.";
    // }

    // angularVelocityCoordinateTransformer(angular_velocity,cameraInfo.sd_card_rotation_);
}

Eigen::VectorXd VirtualGimbalManager::getCorrelationCoefficient(int32_t begin, int32_t length)
{
    if(0 == length){
        length = estimated_angular_velocity->data.rows();
    }
    Eigen::MatrixXd measured_angular_velocity_resampled = measured_angular_velocity->getResampledData(ResamplerParameterPtr(new ResamplerParameter(video_param->getFrequency(), 0, 0)));
    assert(length <= estimated_angular_velocity->data.rows());
    
    Eigen::MatrixXd particial_estimated_angular_velocity = estimated_angular_velocity->data.block(begin,0,length,estimated_angular_velocity->data.cols());
    Eigen::VectorXd particial_confidence = estimated_angular_velocity->confidence.block(begin,0,length,estimated_angular_velocity->confidence.cols());

    int32_t diff = measured_angular_velocity_resampled.rows() - particial_estimated_angular_velocity.rows();
    std::cout << diff << std::endl;
    Eigen::VectorXd correlation_coefficients(diff+1);
    for (int32_t frame = 0, end = correlation_coefficients.rows(); frame < end; ++frame)
    {
        int32_t number_of_data = particial_confidence.cast<int>().array().sum();
        if (0 == number_of_data)
        {
            correlation_coefficients[frame] = std::numeric_limits<double>::max();
        }
        else
        {
            correlation_coefficients[frame] = ((measured_angular_velocity_resampled.block(frame, 0, particial_estimated_angular_velocity.rows(), particial_estimated_angular_velocity.cols()) - particial_estimated_angular_velocity).array().colwise() * particial_confidence.array()).abs().sum() / (double)number_of_data;
        }

        if (frame % 100 == 0)
        {
            printf("\r%d / %d", frame, diff);
            std::cout << std::flush;
        }
    }
    return correlation_coefficients;
}

double VirtualGimbalManager::getSubframeOffset(Eigen::VectorXd &correlation_coefficients, int32_t begin, int32_t length){
    if(0 == length){
        length = estimated_angular_velocity->data.rows();
    }
    assert(length<=estimated_angular_velocity->data.rows());
    // std::cout << correlation_coefficients.block(0,0,100,1) << std::endl;
    std::vector<double> vec_correlation_cofficients(correlation_coefficients.rows());
    Eigen::Map<Eigen::VectorXd>(vec_correlation_cofficients.data(),correlation_coefficients.rows(),1) = correlation_coefficients;
    int32_t minimum_correlation_frame = std::distance(vec_correlation_cofficients.begin(), min_element(vec_correlation_cofficients.begin(), vec_correlation_cofficients.end()));

    Eigen::MatrixXd particial_estimated_angular_velocity = estimated_angular_velocity->data.block(begin,0,length,estimated_angular_velocity->data.cols());
    Eigen::VectorXd particial_confidence = estimated_angular_velocity->confidence.block(begin,0,length,estimated_angular_velocity->confidence.cols());

    //最小値サブピクセル推定
    double minimum_correlation_subframe = 0.0;
    if (minimum_correlation_frame == 0)
    { //位置が最初のフレームで一致している場合
        minimum_correlation_subframe = 0.0;
    }
    else if (minimum_correlation_frame == (correlation_coefficients.rows() - 2))    //なんで2?
    { //末尾
        minimum_correlation_subframe = (double)(correlation_coefficients.rows() - 2);
    }
    else
    {
        std::cout << "minimum_correlation_frame" << minimum_correlation_frame << std::endl;
        double min_value=std::numeric_limits<double>::max();
        int32_t number_of_data = particial_confidence.cast<int>().array().sum();
        for(double sub_frame = -2.0 ; sub_frame<=2.0; sub_frame+=0.001){
            Eigen::MatrixXd measured_angular_velocity_resampled = measured_angular_velocity->getResampledData(std::make_shared<ResamplerParameter>(video_param->getFrequency(), (sub_frame + minimum_correlation_frame) * video_param->getInterval(), estimated_angular_velocity->getInterval()*particial_estimated_angular_velocity.rows()));
            // double value = ((measured_angular_velocity_resampled.block(0, 0, particial_estimated_angular_velocity.rows(), particial_estimated_angular_velocity.cols()) - particial_estimated_angular_velocity).array().colwise() * particial_confidence.array()).abs().sum() / (double)number_of_data;
            assert(measured_angular_velocity_resampled.rows() == particial_estimated_angular_velocity.rows());
            double value = ((measured_angular_velocity_resampled - particial_estimated_angular_velocity).array().colwise() * particial_confidence.array()).abs().sum() / (double)number_of_data;
            if(min_value > value){
                min_value = value;
                minimum_correlation_subframe = sub_frame;
            }
        }
        std::cout << "min_value:" << min_value << std::endl; 
        std::cout << "minimum_correlation_subframe:" << minimum_correlation_subframe;
        // minimum_correlation_subframe = min_value;
    }
    minimum_correlation_subframe += (double)minimum_correlation_frame;
    std::cout << std::endl
              << minimum_correlation_subframe << std::endl;

    return minimum_correlation_subframe * video_param->getInterval() - (estimated_angular_velocity->getInterval() - measured_angular_velocity->getInterval()) * 0.5;
}

void VirtualGimbalManager::setResamplerParameter(double start){
    resampler_parameter_ = std::make_shared<ResamplerParameter>(video_param->getFrequency(), start, estimated_angular_velocity->data.rows() / estimated_angular_velocity->getFrequency());
}

Eigen::MatrixXd VirtualGimbalManager::getSynchronizedMeasuredAngularVelocity()
{
    Eigen::MatrixXd data;
    data.resize(estimated_angular_velocity->data.rows(), estimated_angular_velocity->data.cols() + measured_angular_velocity->data.cols());
    data.block(0, 0, estimated_angular_velocity->data.rows(), estimated_angular_velocity->data.cols()) = estimated_angular_velocity->data;
    Eigen::MatrixXd resampled = measured_angular_velocity->getResampledData(resampler_parameter_);
    assert(estimated_angular_velocity->data.rows() == resampled.rows());
    data.block(0, estimated_angular_velocity->data.cols(), resampled.rows(), resampled.cols()) = resampled;
    return data;
}

Eigen::MatrixXd VirtualGimbalManager::getRotationQuaternions()
{
    Eigen::MatrixXd data;
    data.resize(estimated_angular_velocity->data.rows(), 4);
    rotation_quaternion = std::make_shared<RotationQuaternion>(measured_angular_velocity, *resampler_parameter_);
    for (int i = 0, e = data.rows(); i < e; ++i)
    {

        Eigen::MatrixXd temp = rotation_quaternion->getRotationQuaternion((double)i * video_param->getInterval()).coeffs().transpose();
        // std::cout << rotation_quaternion->getRotationQuaternion((double)i * video_param->getInterval()).coeffs().transpose() << std::endl;
        // std::cout << temp << std::endl << std::flush;
        data.row(i) = temp;
    }
    return data;
}

// void VirtualGimbalManager::getEstimatedAndMeasuredAngularVelocity(Eigen::MatrixXd &data){
//     data.resize(estimated_angular_velocity->data.rows(),estimated_angular_velocity->data.cols()+measured_angular_velocity->data.cols());
//     data.block(0,0,estimated_angular_velocity->data.rows(),estimated_angular_velocity->data.cols()) = estimated_angular_velocity->data;
//     resampler_parameter rp =
//     // data.block(0,estimated_angular_velocity->data.cols(),measured_angular_velocity) =
// }

std::map<int, std::vector<cv::Point2d>> VirtualGimbalManager::getCornerDictionary(cv::Size &pattern_size, bool debug_speedup, bool Verbose)
{
    auto capture = std::make_shared<cv::VideoCapture>(video_param->video_file_name); //動画をオープン
    std::map<int, std::vector<cv::Point2f>> corner_dict;
    cv::Mat gray_image;
    cv::Mat color_image;

    {
        std::vector<cv::Point2f> acquired_image_points;
        cv::TermCriteria criteria(cv::TermCriteria::MAX_ITER | cv::TermCriteria::EPS, 20, 0.001);

        for (int i = 0, e = capture->get(cv::CAP_PROP_FRAME_COUNT); i < e; ++i)
        {
            (*capture) >> color_image;
            cv::cvtColor(color_image, gray_image, cv::COLOR_RGB2GRAY);
            if (cv::findChessboardCorners(gray_image, pattern_size, acquired_image_points, cv::CALIB_CB_FAST_CHECK))
            {
                cv::cornerSubPix(gray_image, acquired_image_points, cv::Size(11, 11), cv::Size(-1, -1), criteria);
                corner_dict[i] = acquired_image_points;
            }
            if (Verbose)
            {
                printf("%d/%d\r", i, e);
                std::cout << std::flush;
            }
            // Speed up for debug
            if (debug_speedup)
            {
                if (i == 100)
                    break;
            }
        }
    }

    std::map<int, std::vector<cv::Point2d>> retval;
    // return corner_dict;
    for (const auto &el : corner_dict)
    {
        for (const auto &el2 : el.second)
        {
            retval[el.first].push_back(cv::Point2d(el2.x, el2.y));
        }
    }
    return retval;
}

Eigen::MatrixXd VirtualGimbalManager::estimateAngularVelocity(const std::map<int, std::vector<cv::Point2d>> &corner_dict, const std::vector<cv::Point3d> &world_points, Eigen::VectorXd &confidence)
{
    cv::Mat CameraMatrix = (cv::Mat_<double>(3, 3) << video_param->camera_info->fx_, 0, video_param->camera_info->cx_, 0, video_param->camera_info->fy_, video_param->camera_info->cy_, 0, 0, 1);
    cv::Mat DistCoeffs = (cv::Mat_<double>(1, 4) << video_param->camera_info->k1_, video_param->camera_info->k2_, video_param->camera_info->p1_, video_param->camera_info->p2_);
    std::map<int, cv::Mat> RotationVector;
    std::map<int, cv::Mat> TranslationVector;

    confidence = Eigen::VectorXd::Zero(video_param->video_frames);

    Eigen::MatrixXd estimated_angular_velocity = Eigen::MatrixXd::Zero(video_param->video_frames, 3);
    for (const auto &el : corner_dict)
    {
        cv::solvePnP(world_points, el.second, CameraMatrix, DistCoeffs, RotationVector[el.first], TranslationVector[el.first]);
        // printf("%d,%f,%f,%f ",el.first,RotationVector[el.first].at<double>(0,0),RotationVector[el.first].at<double>(1,0),RotationVector[el.first].at<double>(2,0));
        // std::cout << "tvec:\r\n" << TranslationVector[el.first] << std::endl << std::flush;
        // std::cout << "rvec:\r\n" << RotationVector[el.first] << std::endl << std::flush;

        Eigen::Quaterniond rotation_quaternion = Vector2Quaternion<double>(
                                                     Eigen::Vector3d(RotationVector[el.first].at<double>(0, 0), RotationVector[el.first].at<double>(1, 0), RotationVector[el.first].at<double>(2, 0)))
                                                     .conjugate();
        // printf("%d,%f,%f,%f,%f,", el.first, rotation_quaternion.x(), rotation_quaternion.y(), rotation_quaternion.z(), rotation_quaternion.w());
        if (0 != RotationVector.count(el.first - 1))
        {
            Eigen::Quaterniond rotation_quaternion_previous = Vector2Quaternion<double>(
                                                                  Eigen::Vector3d(RotationVector[el.first - 1].at<double>(0, 0), RotationVector[el.first - 1].at<double>(1, 0), RotationVector[el.first - 1].at<double>(2, 0)))
                                                                  .conjugate();
            // cv::Mat diff = RotationVector[el.first]-RotationVector[el.first-1];
            Eigen::Quaterniond diff = rotation_quaternion * rotation_quaternion_previous.conjugate();
            // printf("%f,%f,%f\n",diff.at<double>(0,0),diff.at<double>(1,0),diff.at<double>(2,0));
            // printf("%f,%f,%f,%f\n", diff.x(), diff.y(), diff.z(), diff.w());
            Eigen::Vector3d diff_vector = Quaternion2Vector(diff);
            Eigen::Quaterniond estimated_angular_velocity_in_board_coordinate(0.0, diff_vector[0], diff_vector[1], diff_vector[2]);
            Eigen::Quaterniond estimated_angular_velocity_in_camera_coordinate = (rotation_quaternion.conjugate() * estimated_angular_velocity_in_board_coordinate * rotation_quaternion);
            estimated_angular_velocity.row(el.first) << estimated_angular_velocity_in_camera_coordinate.x(), estimated_angular_velocity_in_camera_coordinate.y(),
                estimated_angular_velocity_in_camera_coordinate.z();
            confidence(el.first) = 1.0;
        }
        // else
        // {
        //     printf("0,0,0\n");
        // }
    }
    // std::cout << std::flush;

    return estimated_angular_velocity * video_param->getFrequency();
}

/**
 * @brief Estimate angular velocity from video optical flow
 **/
void VirtualGimbalManager::estimateAngularVelocity(Eigen::MatrixXd &estimated_angular_velocity, Eigen::MatrixXd &confidence)
{
    Eigen::MatrixXd optical_flow;
    if(jsonExists(video_param->video_file_name)){
        readOpticalFlowFromJson(video_param->video_file_name,optical_flow,confidence);
    }else{
        CalcShiftFromVideo(video_param->video_file_name.c_str(), video_param->video_frames, optical_flow, confidence);
    }
    estimated_angular_velocity.resize(optical_flow.rows(), optical_flow.cols());
    estimated_angular_velocity.col(0) =
        optical_flow.col(1).unaryExpr([&](double a) { return video_param->getFrequency() * atan(a / (video_param->camera_info->fy_)); });
    estimated_angular_velocity.col(1) =
        optical_flow.col(0).unaryExpr([&](double a) { return video_param->getFrequency() * -atan(a / (video_param->camera_info->fx_)); });
    estimated_angular_velocity.col(2) = -video_param->getFrequency() * optical_flow.col(2);
}

void VirtualGimbalManager::getUndistortUnrollingChessBoardPoints(double time_offset, const std::pair<int, std::vector<cv::Point2d>> &corner_dict, std::vector<cv::Point2d> &dst, double line_delay)
{
    getUndistortUnrollingChessBoardPoints(corner_dict.first * video_param->getInterval() + time_offset, corner_dict.second, dst, line_delay);
}

/**
 * @brief Undistort and unrolling chess board board points. 
 **/
void VirtualGimbalManager::getUndistortUnrollingChessBoardPoints(double time, const std::vector<cv::Point2d> &src, std::vector<cv::Point2d> &dst, double line_delay)
{

    // Collect time difference between video frame and gyro frame. These frame rates are deferent, so that time should be compensated.
    time += (measured_angular_velocity->getInterval() - estimated_angular_velocity->getInterval()) * 0.5;
    //手順
    //1.補正前画像を分割した時の分割点の座標(pixel)を計算
    //2.1の座標を入力として、各行毎のW(t1,t2)を計算
    //3.補正後の画像上のポリゴン座標(pixel)を計算、歪み補正も含める

    for (const auto &el : src) //(int j = 0; j <= division_y; ++j)
    {
        //W(t1,t2)を計算
        //1
        double v = el.y; //(double)j / division_y * camera_info_.height_;

        double time_in_row = line_delay * (v - video_param->camera_info->height_ * 0.5);
        Eigen::MatrixXd R = (rotation_quaternion->getRotationQuaternion(time_in_row + time).conjugate() * rotation_quaternion->getRotationQuaternion(time)).matrix();
        {
            double u = el.x; //(double)i / division_x * camera_info_.width_;
            //後々の行列演算に備えて、画像上の座標を同次座標で表現しておく。(x座標、y座標,1)T
            Eigen::Vector3d p;
            p << (u - video_param->camera_info->cx_) / video_param->camera_info->fx_, (v - video_param->camera_info->cy_) / video_param->camera_info->fy_, 1.0; // Homogenious coordinate
            //2
            Eigen::MatrixXd XYW = R * p;

            double x1 = XYW(0, 0) / XYW(2, 0);
            double y1 = XYW(1, 0) / XYW(2, 0);

            double r = sqrt(x1 * x1 + y1 * y1);

            double x2 = x1 * (1.0 + video_param->camera_info->inverse_k1_ * r * r + video_param->camera_info->inverse_k2_ * r * r * r * r) + 2.0 * video_param->camera_info->inverse_p1_ * x1 * y1 + video_param->camera_info->inverse_p2_ * (r * r + 2.0 * x1 * x1);
            double y2 = y1 * (1.0 + video_param->camera_info->inverse_k1_ * r * r + video_param->camera_info->inverse_k2_ * r * r * r * r) + video_param->camera_info->inverse_p1_ * (r * r + 2.0 * y1 * y1) + 2.0 * video_param->camera_info->inverse_p2_ * x1 * y1;
            //変な折り返しを防止
            if ((pow(x2 - x1, 2) > 1.0) || (pow(y2 - y1, 2) > 1.0))
            {
                //                printf("折り返し防止\r\n");
                x2 = x1;
                y2 = y1;
            }
            dst.push_back(cv::Point2d(
                x2 * video_param->camera_info->fx_ + video_param->camera_info->cx_,
                y2 * video_param->camera_info->fy_ + video_param->camera_info->cy_));
        }
    }
    return;
}

double VirtualGimbalManager::computeReprojectionErrors(const vector<vector<Point3d>> &objectPoints,
                                                       const vector<vector<Point2d>> &imagePoints,
                                                       const vector<Mat> &rvecs, const vector<Mat> &tvecs,
                                                       const Mat &cameraMatrix, const Mat &distCoeffs,
                                                       vector<double> &residuals, bool fisheye)
{
    vector<Point2d> imagePoints2;
    size_t totalPoints = 0;
    double totalErr = 0, err;
    Point2d diff;

    residuals.resize(imagePoints.size() * imagePoints.begin()->size() * 2);
    auto residuals_itr = residuals.begin();

    for (size_t i = 0; i < objectPoints.size(); ++i)
    {
        if (fisheye)
        {
            fisheye::projectPoints(objectPoints[i], imagePoints2, rvecs[i], tvecs[i], cameraMatrix,
                                   distCoeffs);
        }
        else
        {
            projectPoints(objectPoints[i], rvecs[i], tvecs[i], cameraMatrix, distCoeffs, imagePoints2);
        }
        err = norm(imagePoints[i], imagePoints2, NORM_L2);

        for (size_t k = 0; k < imagePoints2.size(); ++k)
        {
            diff = imagePoints[i][k] - imagePoints2[k];
            *(residuals_itr++) = diff.x;
            *(residuals_itr++) = diff.y;
        }
        size_t n = objectPoints[i].size();
        // residuals[i] = std::sqrt(err * err / n);
        totalErr += err * err;
        totalPoints += n;
    }

    return std::sqrt(totalErr / totalPoints);
}

void VirtualGimbalManager::setFilter(FilterPtr filter)
{
    filter_ = filter;
}

void VirtualGimbalManager::setMaximumGradient(double value)
{
    maximum_gradient_ = value;
}

Eigen::VectorXd VirtualGimbalManager::getFilterCoefficients(double zoom,
                                                            KaiserWindowFilter &filter,
                                                            int32_t strongest_filter_param, int32_t weakest_filter_param)
{

    Eigen::VectorXd filter_strength(video_param->video_frames);
    //Calcurate in all frame
    for (int frame = 0, e = filter_strength.rows(); frame < e; ++frame)
    {
        double time = resampler_parameter_->start + frame * video_param->getInterval();

        // フィルタが弱くて、簡単な条件で、黒帯が出るなら、しょうが無いからこれを採用
        if (hasBlackSpace(time, zoom, measured_angular_velocity, video_param, filter(weakest_filter_param)))
        {
            filter_strength[frame] = weakest_filter_param;
        }
        // フィルタが強くて、すごく安定化された条件で、難しい条件で、黒帯が出ないなら、喜んでこれを採用
        else if (!hasBlackSpace(time, zoom, measured_angular_velocity, video_param, filter(strongest_filter_param)))
        {
            filter_strength[frame] = strongest_filter_param;
        }
        else
        {
            filter_strength[frame] = bisectionMethod(time, zoom, measured_angular_velocity, video_param, filter, strongest_filter_param, weakest_filter_param);
        }
    }
    //    std::cout << filter_strength << std::endl;
    gradientLimit(filter_strength, maximum_gradient_);

    return (filter_strength);
}

std::shared_ptr<cv::VideoCapture> VirtualGimbalManager::getVideoCapture()
{
    return std::make_shared<cv::VideoCapture>(video_param->video_file_name);
}

void VirtualGimbalManager::spin(double zoom, KaiserWindowFilter &filter,Eigen::VectorXd &filter_strength)
{
    // Prepare OpenCL
    cv::ocl::Context context;
    cv::Mat mat_src = cv::Mat::zeros(video_param->camera_info->height_, video_param->camera_info->width_, CV_8UC4); // TODO:冗長なので書き換える
    cv::UMat umat_src = mat_src.getUMat(cv::ACCESS_READ, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
    cv::UMat umat_dst(mat_src.size(), CV_8UC4, cv::ACCESS_WRITE, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
    cv::String build_opt = cv::format("-D dstT=%s", cv::ocl::typeToStr(umat_dst.depth())); // "-D dstT=float"
    initializeCL(context);

   
    // Open Video
    auto capture = getVideoCapture();

    // Stabilize every frames
    std::vector<float> R(video_param->camera_info->height_ * 9); // lines * 3x3 matrix
    float ik1 = video_param->camera_info->inverse_k1_;
        float ik2 = video_param->camera_info->inverse_k2_;
        float ip1 = video_param->camera_info->inverse_p1_;
        float ip2 = video_param->camera_info->inverse_p2_;
        float fx = video_param->camera_info->fx_;
        float fy = video_param->camera_info->fy_;
        float cx = video_param->camera_info->cx_;
        float cy = video_param->camera_info->cy_;


    cv::VideoWriter video_writer = cv::VideoWriter(MultiThreadVideoWriter::getOutputName(video_param->video_file_name.c_str()), cv::VideoWriter::fourcc('F', 'M', 'P', '4'), 23.97, cv::Size(video_param->camera_info->width_, video_param->camera_info->height_), true);
    
    for (int frame = 0; frame <= video_param->video_frames; ++frame)
    {
        // Read a frame image
        (*capture) >> umat_src;
        if(umat_src.empty()){
            break;
        }
        cv::cvtColor(umat_src, umat_src, cv::COLOR_BGR2BGRA);
        
        // Calculate Rotation matrix for every line
        for (int row = 0, e = video_param->camera_info->height_; row < e; ++row)
        {
            double time_in_row = video_param->getInterval() * frame + resampler_parameter_->start + video_param->camera_info->line_delay_ * (row - video_param->camera_info->height_ * 0.5);
            Eigen::Map<Eigen::Matrix<float, 3, 3, Eigen::RowMajor>>(&R[row * 9], 3, 3) = measured_angular_velocity->getCorrectionQuaternion(time_in_row, filter(filter_strength(row)).getFilterCoefficient()).matrix().cast<float>();
        }

        // Send arguments to kernel
        cv::ocl::Image2D image(umat_src);
        cv::ocl::Image2D image_dst(umat_dst, false, true);
        cv::Mat mat_R = cv::Mat(R.size(),1,CV_32F,R.data());
        cv::UMat umat_R = mat_R.getUMat(cv::ACCESS_READ, cv::USAGE_ALLOCATE_DEVICE_MEMORY);

         cv::ocl::Kernel kernel;
        getKernel(kernel_name, kernel_function, kernel, context, build_opt);
        kernel.args(image, image_dst, cv::ocl::KernelArg::ReadOnlyNoSize(umat_R),
        (float)zoom,
        ik1,
        ik2,
        ip1,
        ip2,
        fx,
        fy,
        cx,
        cy);
        size_t globalThreads[3] = {(size_t)mat_src.cols, (size_t)mat_src.rows, 1};
        //size_t localThreads[3] = { 16, 16, 1 };
        bool success = kernel.run(3, globalThreads, NULL, true);
        if (!success)
        {
            cout << "Failed running the kernel..." << endl
                 << flush;
            throw "Failed running the kernel...";
        }

        // cv::Mat mat_for_writer = umat_dst.getMat(cv::ACCESS_READ);
        // if(writer_){

            // writer_->addFrame(mat_for_writer);
        // }


        cv::Mat bgr;
        cv::cvtColor(umat_dst, bgr, cv::COLOR_BGRA2BGR);
        video_writer << bgr;

        // 画面に表示
        cv::UMat small,small_src;
        cv::resize(umat_dst,small,cv::Size(),0.5,0.5);
        // cv::resize(mat_for_writer,small,cv::Size(),0.5,0.5);
        // mat_for_writer.release();
        cv::resize(umat_src,small_src,cv::Size(),0.5,0.5);
        cv::imshow("Original",small_src);
        cv::imshow("Result",small);
        char key = cv::waitKey(1);
        if ('q' == key){
            break;
        }else if('s' == key){
            sleep(1);
            key = cv::waitKey(0);
            if('q' == key){
                cv::destroyAllWindows();
                return;
            }
        }

        //Show fps
        auto t4 = std::chrono::system_clock::now();
        static auto t3 = t4;
        // 処理の経過時間
        double elapsedmicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count() ;
        static double fps = 0.0;
        if(elapsedmicroseconds != 0.0){
            fps = 0.03*(1e6/elapsedmicroseconds) +  0.97*fps;
        }
        t3 = t4;
        printf("fps:%4.2f\r",fps);
        fflush(stdout);
    }
    cv::destroyAllWindows();
    return;
}
void VirtualGimbalManager::enableWriter(const char *video_path){
    writer_ = std::make_shared<MultiThreadVideoWriter>(MultiThreadVideoWriter::getOutputName(video_path),*video_param);
}

std::shared_ptr<ResamplerParameter> VirtualGimbalManager::getResamplerParameterWithClockError(Eigen::VectorXd &correlation_begin, Eigen::VectorXd &correlation_end){
    // TODO: Consider video length is less than 1000
    // Eigen::VectorXd correlation = getCorrelationCoefficient(0,1000);
    correlation_begin = getCorrelationCoefficient(0,1000);
    double offset_begin = getSubframeOffset(correlation_begin,0,1000);
    correlation_end = getCorrelationCoefficient(video_param->video_frames-1000,1000);
    double offset_end = getSubframeOffset(correlation_end,video_param->video_frames-1000,1000);
    double ratio = (offset_end - offset_begin)/((video_param->video_frames - 1000)*video_param->getInterval());
    printf("offset begin: %f, offset end: %f, ratio:%f\r\n",offset_begin,offset_end,ratio);
    return std::make_shared<ResamplerParameter>(video_param->getFrequency(),offset_begin,estimated_angular_velocity->getLengthInSecond());
}