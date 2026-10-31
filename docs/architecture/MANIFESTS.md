# Native 清单源文件与离线校验

`native-source-v1` 是当前 native 子集的可选作者输入约定。独立 JSON Schema 位于 `schemas/native-source-v1.schema.json`，采用 [JSON Schema 2020-12](https://json-schema.org/draft/2020-12/json-schema-validation)。这份源文件约定不冻结内核或运行时 ABI；签名容器、规范 CBOR 与装载检查继续由现有 C 验证器处理。

## 使用

从仓库根目录执行：

```bash
python3 tools/package/sfpack.py manifest-schema --out build/native-source-v1.schema.json
python3 tools/package/sfpack.py validate-manifest build/hello-source.json --kind app
python3 tools/package/sfpack.py app --strict-manifest --manifest build/hello-source.json --image build/hello.elf --seed .keys/application.seed --out build/authored-hello.sfk
```

前两条命令只需要 Python、包工具和仓库 schema，不需要编译好的共享库、ELF 或签名密钥。`validate-manifest` 的结果明确标记为 `source-profile-only`：只确认源文件满足作者约定，不确认映像可运行、内存预算足够、签名受信任或能力获得授权。`module` 和 `app` 的 `--strict-manifest` 启用同一约定；默认仍允许现有的额外元数据。

发布 ZIP 包含上述离线命令所需的 `sfpack.py`、`manifest_profile.py`、`trust_policy.py` 和 schema。解压后从解压目录执行前两条命令即可；源 JSON 和 `--out` 可指向已有的任意目录，例如用 `source.json` 和 `exported-schema.json`，不要求创建 `build`。ZIP 中的 ABI `check` / `diff` 同样只需要 Python。构建、ELF 打包、签名和包认证仍按源码工具链流程执行，发布副本不附带私钥、编译器或认证共享库。

应用源文件示例：

```json
{
  "app_id": "org.spiritfox.hello",
  "version": "0.1.0",
  "runtime_id": "org.spiritfox.native",
  "app_abi": 1,
  "requested_caps": ["stdio"],
  "resource_limits": {"memory": 131072}
}
```

模块最小源文件为 `module_id`、`version` 和 `category`，例如 `org.spiritfox.echo`、`0.1.0`、`service`。能力和资源声明对模块可选；APP 必须声明两者，兼容作者新增的 C 装载要求。实际打包继续检查预算覆盖映像所声明的映射内存。

## 支持的约定

| 字段 | native-source-v1 限制 |
| --- | --- |
| `module_id` / `app_id` | 1–63 个 ASCII 字符，首字符为字母或数字，其余允许字母、数字、点、下划线和短横线 |
| `version` | 三个十进制 uint32 分量，点分隔，无符号、前导零或后缀 |
| `category` | 模块必需，为 `bootstrap`、`driver`、`service` 或 `runtime` |
| `runtime_id` / `app_abi` | APP 必需，分别为 `org.spiritfox.native` 和整数 `1` |
| `requested_caps` | 最多 32 个不同名称，每个 1–64 个 ASCII 字符；首字符为字母或数字，其余另允许点、下划线、冒号和短横线；允许空数组 |
| `resource_limits` | 只包含必需的整数 `memory`，范围 1–33554432 字节；APP 必需，模块可选 |
| `native_profile` | 可省略；模块只允许 `static-v1`，APP 允许 `static-v1` / `relative-v1` |

该约定关闭未知字段，区分 APP 和模块字段，并拒绝作者直接提供 `relative_relocations`、`import_relocations` 或 `native_imports`。PIE 的相对重定位子表由打包器从 ELF 生成。需要现有自由元数据时可使用不带 `--strict-manifest` 的打包命令；程序接口 `pack()` 继续供构建与签名负例使用。

这些约束有意比通用容器更窄，例如能力名称采用 ASCII。它们描述当前工具支持的 native 作者输入，不代表所有可能合法的签名 CBOR 清单，也不赋予能力。内存字段仍是装载准入的声明，不代表已实现通用动态资源配额。

## 所有 JSON 打包输入的界限

默认与严格打包模式都先读取完整、有界、无重复键的 JSON：

- 源文件最多 256 KiB；规范 CBOR 编码最多 64 KiB。
- 根必须是对象；包括键和值在内最多 4096 个节点，根深度为 0，最多 16 层。
- 拒绝任意层的重复键，包括转义后相同的键；拒绝 NUL、无效 UTF-8 和未配对代理字符。
- 拒绝浮点及非有限数字，包括 `1.0`、`1e0`、NaN 和 Infinity；整数必须能编码为现有 CBOR 整数。

以上源文件错误在读取 ELF、读取签名种子、写输出之前拒绝。源约定检查不涉及 ELF 的映射大小；这一检查在随后真实打包中由 C 验证器执行。PIE 自动增加的清单元数据也须通过最终 C 容器检查。源校验或生成包验证失败时，输出文件保持原状；本工具未增加输出文件系统故障下的原子写协议。

仅靠 JSON Schema 无法发现原始 JSON 中已经被解码器覆盖的重复键，也不完整表达源字节数、节点预算或数字词法。其他 JSON Schema 消费者可能把 `1.0` 当作数学整数；本工具的严格源读取仍会拒绝。Python 校验器只实现本仓库固定 schema 使用的约束，不接受任意用户 schema，也不宣称通用 JSON Schema 兼容性。

## 验证与交付

`tests/integration/test_manifest_profile.py` 运行独立数值版本判据、全部边界与真实 CLI。它在缺少 build 和密钥目录的隔离副本中导出 schema、检查模块/APP；用公开固定测试种子打包真实 static、relative-v1 和模块 ELF，并经产品 C 验证及确定性字节对照。缺失映像/种子的负例验证源错误优先拒绝、输出保持；低于映像需要的预算则在真实打包阶段拒绝。

统一测试入口记录 schema 摘要。发布门要求本轮源文件测试标记且 schema 与测试时摘要一致，再把独立 schema 和离线命令的三个 Python 源文件纳入发布 ZIP，其字节摘要记录在 ZIP 清单中；既有 VM 和镜像摘要门保持。完整托管检查必须按当前提交单独核对。
