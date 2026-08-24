#include "CloudCompareCommand.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

TEST (CloudCompareCommand, PinsReleaseAndBuildsCropExport)
{
    evp::CloudCompareCommandRequest request;
    request.executablePath = L"C:\\Tools\\Cloud Compare\\CloudCompare.exe";
    request.logPath = L"C:\\Temp\\tile.log";
    request.inputPath = L"C:\\Survey Files\\tile.laz";
    request.outputPath = L"C:\\Temp Files\\tile output.ply";
    request.cropPolygon = { { 10.0, 20.0 }, { 30.0, 20.0 }, { 30.0, 40.0 } };
    request.keepOutside = true;

    const std::wstring command = evp::BuildCloudCompareCommandLine (request);
    EXPECT_EQ (evp::kCloudComparePinnedVersion, std::string ("2.13.2"));
    EXPECT_EQ (command, L"\"C:\\Tools\\Cloud Compare\\CloudCompare.exe\" -SILENT -AUTO_SAVE OFF "
                        L"-LOG_FILE \"C:\\Temp\\tile.log\" -O -GLOBAL_SHIFT AUTO "
                        L"\"C:\\Survey Files\\tile.laz\" -CROP2D Z -GLOBAL_SHIFT FIRST 3 10 20 30 20 30 40 "
                        L"-OUTSIDE -C_EXPORT_FMT PLY -PLY_EXPORT_FMT BINARY_LE -NO_TIMESTAMP -SAVE_CLOUDS FILE "
                        L"\"\\\"C:\\Temp Files\\tile output.ply\\\"\"");
}

TEST (CloudCompareCommand, OmitsOptionalOperationsWhenDisabled)
{
    evp::CloudCompareCommandRequest request;
    request.executablePath = L"cc.exe";
    request.logPath = L"run.log";
    request.inputPath = L"tile.ply";
    request.outputPath = L"out.ply";

    const std::wstring command = evp::BuildCloudCompareCommandLine (request);
    EXPECT_EQ (command.find (L"-CROP2D"), std::wstring::npos);
    EXPECT_EQ (command.find (L"-OUTSIDE"), std::wstring::npos);
    EXPECT_EQ (command.find (L"-SS"), std::wstring::npos);
}

TEST (CloudCompareCommand, AddsSpatialSubsample)
{
    evp::CloudCompareCommandRequest request;
    request.executablePath = L"cc.exe";
    request.logPath = L"run.log";
    request.inputPath = L"tile.las";
    request.outputPath = L"out.ply";
    request.subsampleStep = 0.125;

    const std::wstring command = evp::BuildCloudCompareCommandLine (request);
    EXPECT_NE (command.find (L"-SS SPATIAL 0.125"), std::wstring::npos);
}

} // namespace
