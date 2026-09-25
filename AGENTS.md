# AGENTS.md — HelloHPC 第二届集群作业指南

本文件供 AI Agent 在后续会话中快速恢复上下文，**不要代替用户执行登录、提交任务等远端操作，除非用户明确要求**。
详细赛题、计分与规则见 `README.md`；集群登录原始文档见
<https://xflops.sjtu.edu.cn/hpc-start-guide/network/login-HPC/> 。

## 背景

- 第二届上海交通大学高性能计算综合能力竞赛（个人赛），正式比赛：2026-09-23 12:00 至 2026-09-28 12:00。
- 赛题位于仓库各编号目录（`03-accelerate` … `11-llm`），每题通常有 `README.md`（题面）与判题脚本。
- 本地自测使用 `hellohpc test`，打包用 `hellohpc pack`；集群预装路径：
  - CPU 集群：`/vault/public/xflops/bin/hellohpc`
  - NPU 集群：`/nfs/bin/hellohpc`
- 评测在容器内进行，环境与集群默认不同（如 GCC 15.2.0、CMake 4.2.3、Python 3.14.4），勿依赖集群默认工具链版本。

## 集群登录

本机账号：`stu1600`（见 `~/.ssh/config`）。

| 用途 | 地址 |
| --- | --- |
| CPU 登录节点 | `armlogin.hpc.sjtu.edu.cn` |
| CPU 计算节点 | 由 `salloc`/`sbatch` 分配，节点名形如 `kp007` |
| NPU 登录节点 | `ascend.xflops.org` |
| NPU 计算节点 | 由作业分配，节点名形如 `ascend6` |

### 首次登录绑定

CPU 集群首次登录若报
`This account have not bind Email/jAccount`，
需到 <https://my.hpc.sjtu.edu.cn/home> 绑定邮箱或 jAccount（推荐邮箱，避免与已有交我算账号冲突）。
NPU 集群无此要求。

### ~/.ssh/config 推荐写法

```sshconfig
Host arm
  HostName armlogin.hpc.sjtu.edu.cn
  User stu1600
  IdentityFile ~/.ssh/id_ed25519
  CertificateFile ~/.ssh/id_ed25519-cert.pub

Host arm_cal
  HostName <计算节点名，如 kp007>
  User stu1600
  ProxyJump arm
  IdentityFile ~/.ssh/id_ed25519
  CertificateFile ~/.ssh/id_ed25519-cert.pub

Host ascend
  HostName ascend.xflops.org
  User stu1600

Host ascend_cal
  HostName <计算节点名，如 ascend6>
  User stu1600
  ProxyJump ascend
```

- 连接：`ssh arm` / `ssh arm_cal` / `ssh ascend` / `ssh ascend_cal`。
- **已知本机隐患**：当前 `~/.ssh/config` 中 `IdentityFile id_ed25519` 为相对路径，ssh 会报
  `no such identity: id_ed25519`，应改为 `~/.ssh/id_ed25519` 与 `~/.ssh/id_ed25519-cert.pub`。
- 本机证书 `~/.ssh/id_ed25519-cert.pub` 有效期至 **2026-10-20 21:09**；过期后需重新在
  <https://my.hpc.sjtu.edu.cn/home> 申请证书（教程：<https://docs.hpc.sjtu.edu.cn/accounts/security.html#id9>）。
- `arm_cal` / `ascend_cal` 的 `HostName` 必须在拿到计算节点后更新。

### VSCode Remote-SSH

- CPU 集群登录使用 `keyboard-interactive`，VSCode 不会自动弹密码框：
  在 Remote.SSH 设置中勾选 **Show Login Terminal**，重连后在终端内输入密码。
- **必须在计算节点上开发**（`arm_cal`）。登录节点有负载限制，跑 VSCode/插件可能触发封号。
- NPU 免密采用默认方式：把本地公钥写入远端 `~/.ssh/authorized_keys` 即可；
  若只有交我算生成的私钥，本地执行 `ssh-keygen -y -f id_ed25519 > id_ed25519.pub` 得到公钥。

## 申请计算资源（Slurm）

CPU 集群：`kp_interact`（≤8 核，最长 8h，适合交互/VSCode）与 `kp_run`（>8 核，最长 2h，最多 128 核，单节点）。
两个队列均需 `-q` 指定 QOS。

```bash
# 交互式
salloc -p kp_interact -q kp_interact -c 8 -t 2:00:00

# 批处理 kp_run
#!/bin/bash
#SBATCH -p kp_run
#SBATCH -q kp_run
#SBATCH -c 32
#SBATCH -t 2:00:00
hostname
```

NPU 集群：

- `contest-slice`：**必须** 12 核 CPU + 1 卡 NPU（`-c 12 --gres=npu:1`），最长 2h，每 4h 最多用 3h。
- `contest-full`：**必须** 192 核 CPU + 8 卡 NPU（`-c 192 --gres=npu:8`），最长 1h，每 3h 最多用 2h；
  需 `LLM Serving` 的 `Stage1` 满分后解锁。

```bash
salloc -p contest-slice -q contest_slice -c 12 --gres=npu:1 -t 2:00:00
salloc -p contest-full  -q contest_full  -c 192 --gres=npu:8 -t 1:00:00
```

### 注意事项与常见报错

- CPU 集群核存比 `1950M`/核，NPU 集群 `4096M`/核，按需申请。
- `salloc` 结束后的终端仍在登录节点，用 `hostname` 确认；计算请在计算节点执行。
- `salloc: ... Requested partition configuration not available now`：检查 `-t` 时限等参数。
- `sbatch` 无报错但作业消失：检查输出重定向目录是否存在。
- **禁止**在登录节点运行并行编译/计算/VSCode 高负载程序；账号仅限本次比赛，滥用会被封号。

## Agent 行为约定

- 远端命令（ssh/salloc/sbatch/scp）属于有副作用操作，执行前先说明用途；用户拒绝后不再重试。
- 本地自测与集群自测结果可能不同，一切以 OJ 评分为准；不得打表、不得异步搬移计时段计算。
- 赛题代码保持原始可读状态，便于赛后 Code Review。
