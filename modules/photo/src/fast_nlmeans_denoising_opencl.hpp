// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

// Copyright (C) 2014, Advanced Micro Devices, Inc., all rights reserved.
// Third party copyrights are property of their respective owners.

#include "precomp.hpp"
#ifndef __OPENCV_FAST_NLMEANS_DENOISING_OPENCL_HPP__
#define __OPENCV_FAST_NLMEANS_DENOISING_OPENCL_HPP__

#include "opencl_kernels_photo.hpp"

#ifdef HAVE_OPENCL

namespace cv {

enum
{
    BLOCK_ROWS = 32,
    BLOCK_COLS = 32,
    CTA_SIZE_INTEL = 64,
    CTA_SIZE_DEFAULT = 256
};

static int divUp(int a, int b)
{
    return (a + b - 1) / b;
}

template <typename FT>
static bool ocl_calcAlmostDist2Weight(UMat & almostDist2Weight, int searchWindowSize, int templateWindowSize, FT h, int cn,
                                      int & almostTemplateWindowSizeSqBinShift, bool abs)
{
    const int maxEstimateSumValue = searchWindowSize * searchWindowSize * 255;
    int fixedPointMult = std::numeric_limits<int>::max() / maxEstimateSumValue;
    int depth = DataType<FT>::depth;
    bool doubleSupport = ocl::Device::getDefault().doubleFPConfig() > 0;

    if (depth == CV_64F && !doubleSupport)
        return false;

    // precalc weight for every possible l2 dist between blocks
    // additional optimization of precalced weights to replace division(averaging) by binary shift
    CV_Assert(templateWindowSize <= 46340); // sqrt(INT_MAX)
    int templateWindowSizeSq = templateWindowSize * templateWindowSize;
    almostTemplateWindowSizeSqBinShift = getNearestPowerOf2(templateWindowSizeSq);
    FT almostDist2ActualDistMultiplier = (FT)(1 << almostTemplateWindowSizeSqBinShift) / templateWindowSizeSq;

    const FT WEIGHT_THRESHOLD = 1e-3f;
    int maxDist = abs ? 255 * cn : 255 * 255 * cn;
    int almostMaxDist = (int)(maxDist / almostDist2ActualDistMultiplier + 1);
    FT den = 1.0f / (h * h * cn);

    almostDist2Weight.create(1, almostMaxDist, CV_32SC1);

    ocl::Kernel k("calcAlmostDist2Weight", ocl::photo::nlmeans_oclsrc,
                  format("-D OP_CALC_WEIGHTS -D FT=%s%s%s", ocl::typeToStr(depth),
                         doubleSupport ? " -D DOUBLE_SUPPORT" : "", abs ? " -D ABS" : ""));
    if (k.empty())
        return false;

    k.args(ocl::KernelArg::PtrWriteOnly(almostDist2Weight), almostMaxDist,
           almostDist2ActualDistMultiplier, fixedPointMult, den, WEIGHT_THRESHOLD);

    size_t globalsize[1] = { almostMaxDist };
    return k.run(1, globalsize, NULL, false);
}

static bool ocl_fastNlMeansDenoising(InputArray _src, OutputArray _dst, float h,
                                     int templateWindowSize, int searchWindowSize, bool abs)
{
    int type = _src.type(), depth = CV_MAT_DEPTH(type), cn = CV_MAT_CN(type);
    int ctaSize = ocl::Device::getDefault().isIntel() ? CTA_SIZE_INTEL : CTA_SIZE_DEFAULT;
    Size size = _src.size();

    if ( type != CV_8UC1 && type != CV_8UC2 && type != CV_8UC3 )
        return false;

    int templateWindowHalfWize = templateWindowSize / 2;
    int searchWindowHalfSize = searchWindowSize / 2;
    templateWindowSize  = templateWindowHalfWize * 2 + 1;
    searchWindowSize = searchWindowHalfSize * 2 + 1;
    int nblocksx = divUp(size.width, BLOCK_COLS), nblocksy = divUp(size.height, BLOCK_ROWS);
    int almostTemplateWindowSizeSqBinShift = -1;

    char cvt[2][40];
    String opts = format("-D OP_CALC_FASTNLMEANS -D TEMPLATE_SIZE=%d -D SEARCH_SIZE=%d"
                         " -D sample_t=%s -D pixel_t=%s -D int_t=%s"
                         " -D BLOCK_COLS=%d -D BLOCK_ROWS=%d"
                         " -D CTA_SIZE=%d -D TEMPLATE_SIZE2=%d -D SEARCH_SIZE2=%d"
                         " -D convert_int_t=%s -D cn=%d -D convert_pixel_t=%s%s",
                         templateWindowSize, searchWindowSize,
                         ocl::typeToStr(depth), ocl::typeToStr(type), ocl::typeToStr(CV_32SC(cn)),
                         BLOCK_COLS, BLOCK_ROWS,
                         ctaSize, templateWindowHalfWize, searchWindowHalfSize,
                         ocl::convertTypeStr(CV_8U, CV_32S, cn, cvt[0]), type == CV_8UC3 ? 4 : cn,
                         ocl::convertTypeStr(CV_32S, CV_8U, cn, cvt[1]), abs ? " -D ABS" : "");

    ocl::Kernel k("fastNlMeansDenoising", ocl::photo::nlmeans_oclsrc, opts);
    if (k.empty())
        return false;

    UMat almostDist2Weight;
    if (!ocl_calcAlmostDist2Weight<float>(almostDist2Weight, searchWindowSize, templateWindowSize,
                                          h, cn, almostTemplateWindowSizeSqBinShift, abs))
        return false;
    CV_Assert(almostTemplateWindowSizeSqBinShift >= 0);

    UMat srcex;
    int borderSize = searchWindowHalfSize + templateWindowHalfWize;
    if (type == CV_8UC3) {
        Mat src_rgb = _src.getMat(), src_rgba(size, CV_8UC4);
        int from_to[] = { 0,0, 1,1, 2,2 };
        mixChannels(&src_rgb, 1, &src_rgba, 1, from_to, 3);
        copyMakeBorder(src_rgba, srcex,
                       borderSize, borderSize, borderSize, borderSize, BORDER_DEFAULT);
    }
    else
        copyMakeBorder(_src, srcex, borderSize, borderSize, borderSize, borderSize, BORDER_DEFAULT);

    _dst.create(size, type);
    UMat dst;
    if (type == CV_8UC3)
        dst.create(size, CV_8UC4);
    else
        dst = _dst.getUMat();

    int searchWindowSizeSq = searchWindowSize * searchWindowSize;
    Size upColSumSize(size.width, searchWindowSizeSq * nblocksy);
    Size colSumSize(nblocksx * templateWindowSize, searchWindowSizeSq * nblocksy);
    UMat buffer(upColSumSize + colSumSize, CV_32SC(cn));

    srcex = srcex(Rect(Point(borderSize, borderSize), size));
    k.args(ocl::KernelArg::ReadOnlyNoSize(srcex), ocl::KernelArg::WriteOnly(dst),
           ocl::KernelArg::PtrReadOnly(almostDist2Weight),
           ocl::KernelArg::PtrReadOnly(buffer), almostTemplateWindowSizeSqBinShift);

    size_t globalsize[2] = { nblocksx * ctaSize, nblocksy }, localsize[2] = { ctaSize, 1 };
    if (!k.run(2, globalsize, localsize, false)) return false;

    if (type == CV_8UC3) {
        Mat dst_rgba = dst.getMat(ACCESS_READ), dst_rgb = _dst.getMat();
        int from_to[] = { 0,0, 1,1, 2,2 };
        mixChannels(&dst_rgba, 1, &dst_rgb, 1, from_to, 3);
    }

    return true;
}

static bool ocl_fastNlMeansDenoisingColored( InputArray _src, OutputArray _dst,
                                      float h, float hForColorComponents,
                                      int templateWindowSize, int searchWindowSize)
{
    UMat src = _src.getUMat();
    _dst.create(src.size(), src.type());
    UMat dst = _dst.getUMat();

    UMat src_lab;
    cvtColor(src, src_lab, COLOR_LBGR2Lab);

    UMat l(src.size(), CV_8U);
    UMat ab(src.size(), CV_8UC2);
    std::vector<UMat> l_ab(2), l_ab_denoised(2);
    l_ab[0] = l;
    l_ab[1] = ab;
    l_ab_denoised[0].create(src.size(), CV_8U);
    l_ab_denoised[1].create(src.size(), CV_8UC2);

    int from_to[] = { 0,0, 1,1, 2,2 };
    mixChannels(std::vector<UMat>(1, src_lab), l_ab, from_to, 3);

    fastNlMeansDenoising(l_ab[0], l_ab_denoised[0], h, templateWindowSize, searchWindowSize);
    fastNlMeansDenoising(l_ab[1], l_ab_denoised[1], hForColorComponents, templateWindowSize, searchWindowSize);

    UMat dst_lab(src.size(), CV_8UC3);
    mixChannels(l_ab_denoised, std::vector<UMat>(1, dst_lab), from_to, 3);

    cvtColor(dst_lab, dst, COLOR_Lab2LBGR, src.channels());
    return true;
}

}

#endif
#endif
