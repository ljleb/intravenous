#include "module_test_utils.h"

#include <gtest/gtest.h>

TEST(ModuleLoaderVariants, LoadsSingleModuleCppPath)
{
    auto const fixtures = iv::test::test_modules_root();

    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::test::make_executor(
        loader,
        audio_device,
        std::move(execution_targets),
        1,
        fixtures / "nested_loader_project" / "module.cpp"
    );
    iv::test::run_processor_ticks(audio_device, executor);
}

TEST(ModuleLoaderVariants, LoadsDirectoryWithLocalCmake)
{
    auto const fixtures = iv::test::test_modules_root();

    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader();
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::test::make_executor(
        loader,
        audio_device,
        std::move(execution_targets),
        1,
        fixtures / "local_cmake"
    );
    iv::test::run_processor_ticks(audio_device, executor);
}

TEST(ModuleLoaderVariants, LoadsMovedModuleTree)
{
    auto const fixtures = iv::test::test_modules_root();
    auto moved_root = iv::test::runtime_modules_root() / "moved_modules";
    std::filesystem::remove_all(moved_root);
    iv::test::copy_directory(fixtures / "nested_loader_project", moved_root / "project");
    iv::test::copy_directory(fixtures / "nested_loader_voice", moved_root / "voice");

    iv::test::FakeAudioDevice audio_device;
    auto loader = iv::test::make_loader({ moved_root });
    iv::ExecutionTargetRegistry execution_targets(iv::test::make_audio_device_provider(audio_device));
    iv::NodeExecutor executor = iv::test::make_executor(
        loader,
        audio_device,
        std::move(execution_targets),
        1,
        moved_root / "project"
    );
    iv::test::run_processor_ticks(audio_device, executor);
}
