# SpiritFoxOS 开发约定

- 协作目标环境包括 Arch Linux x86_64。Windows 上通过 Linux 虚拟机编译；不把 Windows 本机编译结果作为 Linux 验证结果。
- 源码使用 UTF-8、LF；Makefile 配方保留 Tab，Shell 脚本保留 Git 的可执行位。不要批量改格式或引入大小写冲突。
- 路径从项目根目录计算，个人路径放被忽略的 `.spiritfox-local.json`。生成的库、头文件、磁盘镜像和日志留在被忽略的构建目录。
- 第三方依赖版本、来源和 SHA256 由 `tools/deps.lock` 记录；不要提交仅存在于开发者机器的库文件。
- 修改构建逻辑时兼容 Arch 的原生 GCC 与 Debian/Ubuntu 的工具命名，保留显式 `CROSS` 和固件变量覆盖能力。
- 推送前验证将要推送的提交，而非仅验证可能含未提交修改的工作区。启用 `.githooks/pre-push`；禁止为赶进度跳过失败检查。Windows 使用 `tools/dev.ps1 verify-ref <commit>`，Linux 使用归档快照执行 `tools/check-linux.sh`。
- 通过编译和启动检查只代表对应范围通过，不能宣称所有应用、驱动或真机硬件已经验证。
- 个人虚拟机、密钥和下载缓存不进入仓库。不要将仓库外的个人环境路径提交给其他开发者。
