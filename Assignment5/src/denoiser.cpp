#include "denoiser.h"

Denoiser::Denoiser() : m_useTemportal(false) {}

void Denoiser::Reprojection(const FrameInfo &frameInfo) {
    int height = m_accColor.m_height;
    int width = m_accColor.m_width;
    Matrix4x4 preWorldToScreen =
            m_preFrameInfo.m_matrix[m_preFrameInfo.m_matrix.size() - 1];
    Matrix4x4 preWorldToCamera =
            m_preFrameInfo.m_matrix[m_preFrameInfo.m_matrix.size() - 2];
#pragma omp parallel for
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // TODO: Reproject
            // 公式参考：https://zhuanlan.zhihu.com/p/607012514
            // screen(pre) = P(pre) * V(pre) * M(pre) * M(cur) * position(cur)
            m_valid(x, y) = false;
            m_misc(x, y) = Float3(0.f);

            // 公式带入计算
            int currentId = frameInfo.m_id(x, y);
            // invalid pixel
            if (currentId == -1) {
                continue;
            }
            Float3 position = frameInfo.m_position(x, y);                                    // position(cur)
            Matrix4x4 curWorldToLocal = Inverse(frameInfo.m_matrix[currentId]);         // M(cur)
            Matrix4x4 preLocalToWorld = m_preFrameInfo.m_matrix[currentId];                  // M(pre)
            Float3 preLocal = curWorldToLocal(position, Float3::EType::Point);      // M(cur) * position(cur)
            Float3 preWorld = preLocalToWorld(preLocal, Float3::EType::Point);      // M(pre) * M(cur) * position(cur)
            Float3 preScreen = preWorldToScreen(preWorld, Float3::EType::Point);    // P(pre) * V(pre) * M(pre) * M(cur) * position(cur)

            // 1. 判断是否在屏幕内 -- m_valid
            // 2. 上一帧与当前帧的物体标号相同
            if (preScreen.x >= 0 && preScreen.x < width && preScreen.y >= 0 && preScreen.y < height) {
                int preX = preScreen.x;
                int preY = preScreen.y;
                int preId = m_preFrameInfo.m_id(preX, preY);
                if (preId == currentId) {
                    m_valid(x, y) = true;
                    m_misc(x, y) = m_accColor(preX, preY);
                }
            }
        }
    }
    std::swap(m_misc, m_accColor);
}

void Denoiser::TemporalAccumulation(const Buffer2D<Float3> &curFilteredColor) {
    int height = m_accColor.m_height;
    int width = m_accColor.m_width;
    int kernelRadius = 3;
#pragma omp parallel for
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // TODO: Temporal clamp
            Float3 preFilteredColor = m_accColor(x, y);
            // TODO: Exponential moving average
            float alpha = 1.0f;

            // 公式: preFilteredColor = alpha * curFilteredColor + (1 - alpha) * clamp(preFilteredColor, mu - sigma * k, mu + sigma * k)
            if (m_valid(x, y)) {
                alpha = m_alpha;

                Float3 mu(0.f);
                Float3 sigma2(0.f);
                int x_start = std::max(0, x - kernelRadius);
                int x_end = std::min(width - 1, x + kernelRadius);
                int y_start = std::max(0, y - kernelRadius);
                int y_end = std::min(height - 1, y + kernelRadius);

                for (int nx = x_start; nx <= x_end; nx++) {
                    for (int ny = y_start; ny <= y_end; ny++) {
                        mu += curFilteredColor(nx, ny);
                        sigma2 += Sqr(curFilteredColor(x, y) - curFilteredColor(nx, ny));
                    }
                }

                int count = (2 * kernelRadius + 1) * (2 * kernelRadius + 1);
                mu /= (float) count;
                Float3 sigma = SafeSqrt(sigma2 / (float) count);
                preFilteredColor = Clamp(preFilteredColor, mu - sigma * m_colorBoxK, mu + sigma * m_colorBoxK);
            }

            m_misc(x, y) = Lerp(preFilteredColor, curFilteredColor(x, y), alpha);
        }
    }
    std::swap(m_misc, m_accColor);
}

Buffer2D<Float3> Denoiser::Filter(const FrameInfo &frameInfo) {
    int height = frameInfo.m_beauty.m_height;
    int width = frameInfo.m_beauty.m_width;
    Buffer2D<Float3> filteredImage = CreateBuffer2D<Float3>(width, height);
    int kernelRadius = 16;
#pragma omp parallel for
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // TODO: Joint bilateral filter
            // filteredImage(x, y) = frameInfo.m_beauty(x, y);
            Float3 finalColor = Float3(0.f);
            float weightSum = 0.f;

            int x_start = std::max(0, x - kernelRadius);
            int x_end = std::min(width - 1, x + kernelRadius);
            int y_start = std::max(0, y - kernelRadius);
            int y_end = std::min(height - 1, y + kernelRadius);

            Float3 centerColor = frameInfo.m_beauty(x, y);
            Float3 centerNormal = frameInfo.m_normal(x, y);
            Float3 centerPosition = frameInfo.m_position(x, y);

            for (int nx = x_start; nx <= x_end; nx++) {
                for (int ny = y_start; ny <= y_end; ny++) {

                    Float3 color = frameInfo.m_beauty(nx, ny);
                    Float3 normal = frameInfo.m_normal(nx, ny);
                    Float3 position = frameInfo.m_position(nx, ny);

                    // Ref: https://zhuanlan.zhihu.com/p/607012514
                    // calculate distance
                    float colorDis = SqrDistance(centerColor, color) / (2.f * m_sigmaColor * m_sigmaColor);
                    float positionDis = SqrDistance(centerPosition, position) / (2.f * m_sigmaCoord * m_sigmaCoord);
                    float normalDis = SafeAcos(Dot(centerNormal, normal)) * SafeAcos(Dot(centerNormal, normal)) / (2.f * m_sigmaNormal * m_sigmaNormal);
                    float planeDis = 0.f;
                    if (positionDis > 0.f) {
                        planeDis = Dot(centerPosition, Normalize(position - centerPosition)) * Dot(centerPosition, Normalize(position - centerPosition)) / (2.f * m_sigmaPlane * m_sigmaPlane);
                    }

                    float weight = std::exp(-colorDis - positionDis - normalDis - planeDis);
                    finalColor += color * weight;
                    weightSum += weight;
                }
            }
            if (weightSum == 0.f) {
                filteredImage(x, y) = centerColor;
                continue;
            }
            filteredImage(x, y) = finalColor / weightSum;
        }
    }
    return filteredImage;
}

void Denoiser::Init(const FrameInfo &frameInfo, const Buffer2D<Float3> &filteredColor) {
    m_accColor.Copy(filteredColor);
    int height = m_accColor.m_height;
    int width = m_accColor.m_width;
    m_misc = CreateBuffer2D<Float3>(width, height);
    m_valid = CreateBuffer2D<bool>(width, height);
}

void Denoiser::Maintain(const FrameInfo &frameInfo) { m_preFrameInfo = frameInfo; }

Buffer2D<Float3> Denoiser::ProcessFrame(const FrameInfo &frameInfo) {
    // Filter current frame
    Buffer2D<Float3> filteredColor;
    filteredColor = Filter(frameInfo);

    // Reproject previous frame color to current
    if (m_useTemportal) {
        Reprojection(frameInfo);
        TemporalAccumulation(filteredColor);
    } else {
        Init(frameInfo, filteredColor);
    }

    // Maintain
    Maintain(frameInfo);
    if (!m_useTemportal) {
        m_useTemportal = false;
    }
    return m_accColor;
}
