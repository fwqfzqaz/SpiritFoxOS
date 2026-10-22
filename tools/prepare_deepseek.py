"""Adapt the user's existing bridge without changing its source or credentials."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = Path('E:/code/tools/dsh-bridge/bridge.cjs').read_text(encoding='utf-8')
source = source.replace("const home = path.join(__dirname, 'data');", "const home = 'E:/SpiritFoxOS/.cache/deepseek/home';")
source = source.replace("const tempRoot = 'E:/code/tmp/dsh-bridge';", "const tempRoot = 'E:/SpiritFoxOS/.cache/deepseek/tmp';")
source = source.replace("const workspaceRoot = 'E:/code/work/dsh-tasks';", "const workspaceRoot = 'E:/SpiritFoxOS';")
source = source.replace("const runsRoot = 'E:/code/work/DSH接入/runs';", "const runsRoot = 'E:/SpiritFoxOS/.cache/deepseek/runs';")
source = source.replace(" || workspace === fs.realpathSync.native(workspaceRoot)", '')
dest = root / '.cache' / 'deepseek'
dest.mkdir(parents=True, exist_ok=True)
(dest / 'bridge.cjs').write_text(source, encoding='utf-8')
(dest / 'review.txt').write_text('只读审查 E:/SpiritFoxOS/kernel、include、services 中现有 C 代码。寻找可以具体证明的内存/权限/生命周期缺陷，最多报告八个，每个给出文件名、函数、触发方式和建议。代码尚未编译；不要讨论未实现的未来功能。不要改文件，不读取工作区外文件，不执行命令，不访问网络，不提交推送。', encoding='utf-8')
