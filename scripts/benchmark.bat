setlocal
pushd "%~dp0"
cmake --build ..\build-release --target intravenous_render_benchmark
..\build-release\benchmark\intravenous_render_benchmark.exe ..\tests\test_modules\noisy_saw_project --block-size 256 --warmup-blocks 200 --min-blocks 1000 > benchmark.txt.2 && move /y benchmark.txt.2 benchmark.txt
