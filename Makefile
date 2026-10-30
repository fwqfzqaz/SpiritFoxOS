# SpiritFoxOS 顶层 Make 入口。
#
# 真正的构建、测试与打包逻辑仍在 tools/*.py 中。这里只提供统一、可发现
# 的命令行入口，不复制任何构建规则；所有命令都从仓库根目录执行。

.DEFAULT_GOAL := help

# Windows 使用 python，其余平台使用 python3；可用 make PYTHON=... 覆盖。
PYTHON ?= $(if $(filter Windows_NT,$(OS)),python,python3)
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

BUILD_DIR ?= build
TRUST_POLICY ?=
PACKAGE ?= build/hello.sfk
TEST_ARGS ?=
QEMU_ARGS ?=

BUILD_ARGS := $(if $(strip $(TRUST_POLICY)),--trust-policy "$(TRUST_POLICY)")

.PHONY: help all bootstrap setup build test test-host test-reuse \
        run run-persist run-iommu qemu-test qemu-list \
        release dist inspect show-trust \
        check-build check-toolchain clean distclean clean-tools

help: ## 显示本帮助
	@$(PYTHON) -c "import re; [print('  ' + m.group(1).ljust(26) + m.group(2)) for m in (re.match(r'^([A-Za-z0-9_.-]+):.*## (.*)$$', line) for line in open('$(THIS_MAKEFILE)', encoding='utf-8')) if m]; print(); print('  ' + 'qemu-<场景>'.ljust(26) + '运行 tests/qemu/qemu_<场景>_test.py，例如 make qemu-trust；用 make qemu-list 查看全部')"

all: build ## build 的别名

bootstrap: ## 下载/校验 Zig 与 Monocypher，检查系统 QEMU/OVMF
	$(PYTHON) tools/bootstrap.py

setup: bootstrap ## bootstrap 的别名

build: check-toolchain ## 构建内核、UEFI 镜像、签名包与测试盘
	$(PYTHON) tools/build.py $(BUILD_ARGS)

check-toolchain:
	@$(PYTHON) -c "import os,sys; paths=(['.tools/zig/zig.exe'] if os.name == 'nt' else ['.tools/zig/zig'])+['vendor/monocypher/monocypher.c']; missing=[p for p in paths if not os.path.exists(p)]; sys.exit('missing '+', '.join(missing)+'; run make bootstrap first') if missing else None"

check-build:
	@$(PYTHON) -c "import os,sys; f='$(BUILD_DIR)/esp/EFI/BOOT/BOOTX64.EFI'; sys.exit(0) if os.path.exists(f) else sys.exit('missing '+f+'; run make build first')"

test: check-toolchain ## 构建并运行宿主、集成与全部 QEMU 测试
	$(PYTHON) tools/test.py $(TEST_ARGS)

test-host: check-toolchain ## 构建并只运行宿主与集成测试（不含 QEMU）
	$(PYTHON) tools/test.py --host-only $(TEST_ARGS)

test-reuse: check-toolchain ## 复用已有镜像重跑宿主、集成与 QEMU 测试（--skip-build）
	$(PYTHON) tools/test.py --skip-build $(TEST_ARGS)

run: check-build ## 交互启动：只读测试盘，默认 256 MiB
	$(PYTHON) tests/qemu/qemu_test.py --interactive $(QEMU_ARGS)

run-persist: check-build ## 交互启动并使用可写 build/user-persistent.img
	$(PYTHON) tests/qemu/qemu_test.py --interactive --persistent $(QEMU_ARGS)

run-iommu: check-build ## run-persist + Intel VT-d 与 modern virtio
	$(PYTHON) tests/qemu/qemu_test.py --interactive --persistent --iommu $(QEMU_ARGS)

qemu-test: check-build ## 非交互运行基础场景 tests/qemu/qemu_test.py
	$(PYTHON) tests/qemu/qemu_test.py $(QEMU_ARGS)

qemu-%: check-build ## 运行单个场景，例如 make qemu-trust / qemu-file-rpc
	$(PYTHON) tests/qemu/qemu_$(subst -,_,$*)_test.py $(QEMU_ARGS)

qemu-list: ## 列出可用的 qemu-<场景> 目标
	@$(PYTHON) -c "import pathlib; [print('  qemu-' + p.stem[len('qemu_'):-len('_test')].replace('_', '-') + '  (tests/qemu/' + p.name + ')') for p in sorted(pathlib.Path('tests/qemu').glob('qemu_*_test.py')) if p.name != 'qemu_test.py']"

release: ## 依据已通过的完整测试结果生成发布 ZIP
	$(PYTHON) tools/package_release.py

dist: test ## 先跑完整测试，再生成发布 ZIP
	$(PYTHON) tools/package_release.py

inspect: check-build ## 用镜像信任策略检查包：make inspect PACKAGE=build/hello.sfk
	$(PYTHON) tools/package/sfpack.py inspect "$(PACKAGE)" --trust-policy "$(BUILD_DIR)/trust-policy.json"

show-trust: check-build ## 打印镜像中实际生效的信任策略
	$(PYTHON) tools/package/sfpack.py trust --policy "$(BUILD_DIR)/trust-policy.json"

clean: ## 删除 build/（保留 .keys、.tools 与 .cache）
	@$(PYTHON) -c "import shutil; shutil.rmtree('$(BUILD_DIR)', ignore_errors=True)"

distclean: clean ## 额外删除 .cache 与 __pycache__
	@$(PYTHON) -c "import pathlib, shutil; [shutil.rmtree(p, ignore_errors=True) for p in [pathlib.Path('.cache'), *sorted(pathlib.Path('.').rglob('__pycache__'))]]"

clean-tools: ## 删除 .tools（下次 bootstrap 会重新下载 Zig/QEMU）
	@$(PYTHON) -c "import shutil; shutil.rmtree('.tools', ignore_errors=True)"
