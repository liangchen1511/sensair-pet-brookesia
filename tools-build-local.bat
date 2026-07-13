@echo off
set "IDF_PATH=D:\esp32\Espressif\v5.5.4\esp-idf"
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.4\venv"
set "IDF_PYTHON_CHECK_CONSTRAINTS=no"
set "PYTHONUTF8=1"
set "SENSAIR_LOCAL_TOOLCHAIN=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin"
set PATH=C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;%PATH%
set "CC=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"
set "CXX=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-g++.exe"
set "ASM=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe"
where riscv32-esp-elf-gcc.exe >nul 2>nul || (
  echo C5 toolchain is not available in PATH
  exit /b 1
)
"C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" ^
  -D "CMAKE_MAKE_PROGRAM=C:\Espressif\tools\ninja\1.12.1\ninja.exe" ^
  -D "CMAKE_C_COMPILER=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe" ^
  -D "CMAKE_CXX_COMPILER=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-g++.exe" ^
  -D "CMAKE_ASM_COMPILER=C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe" %*
