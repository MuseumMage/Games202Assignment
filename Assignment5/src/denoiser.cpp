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
            m_valid(x, y) = false;
            m_misc(x, y) = Float3(0.f);
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
            Float3 color = m_accColor(x, y);
            // TODO: Exponential moving average
            float alpha = 1.0f;
            m_misc(x, y) = Lerp(color, curFilteredColor(x, y), alpha);
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
            for (int j = 0; j < kernelRadius; j++)
            {
                for (int i = 0; i < kernelRadius; i++)
                {
                    int nx = std::min(std::max(x + i - kernelRadius / 2, 0), width);
                    int ny = std::min(std::max(y + j - kernelRadius / 2, 0), height);

                    Float3 color = frameInfo.m_beauty(nx, ny);
                    Float3 normal = frameInfo.m_normal(nx, ny);
                    Float3 position = frameInfo.m_position(nx, ny);

                    Float3 centerColor = frameInfo.m_beauty(x, y);
                    Float3 centerNormal = frameInfo.m_normal(x, y);
                    Float3 centerPosition = frameInfo.m_position(x, y);

                    // Ref: https://zhuanlan.zhihu.com/p/607012514
                    // calculate distance
                    float colorDis = SqrDistance(centerColor, color) / (2 * m_sigmaColor * m_sigmaColor);
                    float positionDis = SqrDistance(centerPosition, position) / (2 * m_sigmaCoord * m_sigmaCoord);
                    float normalDis = SafeAcos(Dot(centerNormal, normal)) * SafeAcos(Dot(centerNormal, normal)) / (2 * m_sigmaNormal * m_sigmaNormal);
                    if (positionDis <= 0)
                    {
                        continue;
                    }
                    float planeDis = Dot(centerPosition, Normalize(position - centerPosition)) / (2 * m_sigmaPlane * m_sigmaPlane);

                    float weight = std::exp(-colorDis - positionDis - normalDis - planeDis);
                    finalColor += color * weight;
                    weightSum += weight;
                }
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
        m_useTemportal = true;
    }
    return m_accColor;
}
