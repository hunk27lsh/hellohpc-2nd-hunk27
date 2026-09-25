# HelloHPC 第二届 Writeup

## 2.4 [5'*2] kp_run 队列节点实际使用的以太网卡名称与固件版本

**答案**

- 实际使用的以太网卡名称：`enp189s0f0`
- 网卡固件版本：`1.8.15.0`

**环境**：登录 CPU 集群登录节点（`ssh arm`，账号 `stu1600`），作业落在 `kp_run` 队列节点 `kp027` 上。

### 实际操作过程

1. 在登录节点 `kp001` 申请 `kp_run` 队列资源：

```text
[stu1600@kp001 ~]$ salloc -p kp_run -q kp_run -c 16 -t 0:10:00
salloc: Pending job allocation 62954598
salloc: job 62954598 queued and waiting for resources
salloc: job 62954598 has been allocated resources
salloc: Granted job allocation 62954598
salloc: Nodes kp027 are ready for job
bash-5.1$ hostname
kp001.pi.sjtu.edu.cn
bash-5.1$ ip route get 8.8.8.8
8.8.8.8 via 202.120.58.254 dev enp189s0f1 src 202.120.58.251
```

`salloc` 虽然分配到了 `kp027`，但当前 shell 仍在登录节点 `kp001` 上（`hostname` 可证），此时查到的 `enp189s0f1` 是登录节点的网卡，不是本题要的 kp_run 节点答案。

2. 在分配到的作业内启动计算节点上的 shell，并确认节点：

```text
bash-5.1$ srun --pty bash
bash-5.1$ hostname
kp027.pi.sjtu.edu.cn
```

3. 查看实际使用的网卡。

先尝试用默认路由找出口网卡，但计算节点没有外网默认路由：

```text
bash-5.1$ ip route get 8.8.8.8
RTNETLINK answers: Network is unreachable
```

于是直接查看网卡与路由表：

```text
bash-5.1$ ip -br addr
lo               UNKNOWN        127.0.0.1/8 ::1/128
enp189s0f0       UP             172.16.32.27/16 fe80::12c3:abff:fe50:ca25/64
enp189s0f1       DOWN
enp125s0f0       DOWN
ibp1s0           DOWN

bash-5.1$ ip route
10.119.3.66 via 172.16.0.133 dev enp189s0f0 proto static metric 100
10.119.9.12 via 172.16.0.133 dev enp189s0f0 proto static metric 100
111.186.38.22 via 172.16.0.133 dev enp189s0f0 proto static metric 100
111.186.59.34 via 172.16.0.133 dev enp189s0f0 proto static metric 100
172.16.0.0/16 dev enp189s0f0 proto kernel scope link src 172.16.32.27 metric 100
192.168.0.0/16 via 172.16.0.1 dev enp189s0f0 proto static metric 100
202.121.181.107 via 172.16.0.133 dev enp189s0f0 proto static metric 100
202.121.181.130 via 172.16.0.133 dev enp189s0f0 proto static metric 100

bash-5.1$ hostname -I
172.16.32.27
```

所有路由都从 `enp189s0f0` 出去，且只有它处于 UP 状态并持有本机 IP，因此它就是实际使用的以太网卡。

4. 查询该网卡的固件版本：

```text
bash-5.1$ ethtool -i enp189s0f0
driver: hns3
version: 5.10.0-218.0.0.121.oe2203sp3.aa
firmware-version: 1.8.15.0
expansion-rom-version:
bus-info: 0000:bd:00.0
supports-statistics: yes
supports-test: yes
supports-eeprom-access: no
supports-register-dump: yes
supports-priv-flags: yes
```

`firmware-version` 即固件版本 `1.8.15.0`。

### 输出解析

- `salloc -p kp_run -q kp_run -c 16 -t 0:10:00`：向 `kp_run` 分区（QOS 也为 `kp_run`）申请 16 核、最长 10 分钟的资源。`salloc` 只负责分配资源，**不会自动把当前 shell 放到计算节点上**，所以需要再执行 `srun --pty bash`（或 `ssh kp027`）才真正进入 `kp027`。
- `ip route get 8.8.8.8`：询问内核"去 8.8.8.8 会走哪条路径"。登录节点有默认路由，输出 `via 202.120.58.254 dev enp189s0f1`，即登录节点用 `enp189s0f1` 出网；而计算节点没有默认路由，直接报 `RTNETLINK answers: Network is unreachable`。这正说明题目必须在计算节点上做，两个节点的网卡并不相同。
- `ip -br addr`：简洁列出各接口的状态与地址。`lo` 是环回口；`enp189s0f0` 为 UP 且持有 `172.16.32.27/16` 和 IPv6 链路本地地址，是在用的物理网卡；`enp189s0f1`、`enp125s0f0`、`ibp1s0`（InfiniBand）均为 DOWN。以太网卡名 `enp189s0f0` 属于可预测命名：`en`=Ethernet，`p189`=PCI 总线号，`s0`=slot 0，`f0`=function 0。
- `ip route`：路由表证实所有条目都经 `dev enp189s0f0`，其中 `172.16.0.0/16` 是直连子网（`scope link`，源地址 `172.16.32.27`），其余内网/互通地址走网关 `172.16.0.133`；表中没有 `default` 条目，与上一步"不可达"一致。
- `hostname -I`：输出本机 IP `172.16.32.27`，与 `enp189s0f0` 上的地址互相印证。
- `ethtool -i enp189s0f0`：打印网卡的驱动与硬件信息。
  - `driver: hns3`：驱动模块名（华为海思 HNS3 系列网卡）；
  - `version: 5.10.0-218...oe2203sp3.aa`：驱动版本，跟随系统内核（openEuler 22.03 SP3）；
  - `firmware-version: 1.8.15.0`：**网卡固件版本，正是本题所求**；
  - `bus-info: 0000:bd:00.0`：网卡 PCI 地址，可用于 `lspci -s bd:00.0` 进一步确认型号。

## 2.5 [5'*2] kp_run 队列节点的默认 C/C++ 编译器版本与主程序入口地址

**答案**

- 默认 C/C++ 编译器版本：`10.3.1`（集群默认版本，与 `gcc --version` 首行输出一致）
- 主程序入口地址：`0x404440`（指 gcc 编译器本体的入口，不是被编译程序的入口）

**环境**：复用 2.4 题的 `kp_run` 作业会话，仍在计算节点 `kp027`（`srun --pty bash`）中操作。

### 实际操作过程

1. 查询默认编译器版本（`gcc` 与 `g++` 同属 GCC 10.3.1）：

```text
bash-5.1$ gcc --version | head -1
gcc (GCC) 10.3.1
```

2. 最初把"主程序"理解为自己编写的测试程序，编译 `hello.c` 后读取它的 ELF 入口：

```text
bash-5.1$ cat > hello.c <<'EOF'
> int main(void) { return 0; }
> EOF
bash-5.1$ gcc hello.c -o hello
bash-5.1$ readelf -h hello | grep 'Entry point'
  Entry point address:               0x400500
```

该值提交后被判错。原因：题目中的"其主程序"指的是**编译器自身的主程序**（即 `gcc` 驱动程序这个可执行文件），而不是被编译出来的程序，"其"指代的是前半句的"默认 C/C++ 编译器"。

3. 找到编译器本体并读取它的入口地址：

```text
bash-5.1$ realpath "$(which gcc)"
/usr/bin/aarch64-openEuler-linux-gnu-gcc-10.3.1
bash-5.1$ readelf -h "$(realpath "$(which gcc)")" | grep -E '入口点|Entry point'
  入口点地址：              0x404440
```

即正确答案为 `0x404440`。

### 输出解析

- `which gcc` 给出的是 `/usr/bin/gcc`（通常是指向具体版本可执行文件的符号链接），`realpath` 将其解析为真正的 ELF 可执行文件；集群（CPU 集群为 ARM，登录名 `armlogin`）上是 AArch64 架构的 `aarch64-openEuler-linux-gnu-gcc-10.3.1`。
- `readelf -h <文件>` 打印 ELF 头，`入口点地址 / Entry point address` 即 ELF 中的 `e_entry` 字段——内核加载该可执行文件后跳转执行的第一条指令地址。编译器本身也是一个 ELF 可执行程序，同样有入口地址。
- 自己编译的 `hello` 的入口（`0x400500`）与编译器本体的入口（`0x404440`）是两个不同程序的地址，前者不是本题答案。题目两空的主语都是"默认 C/C++ 编译器"，第二空应针对 `gcc` 可执行文件本身执行 `readelf`。
- 交叉核验：openEuler 22.03 LTS SP3 仓库中 `gcc-10.3.1-49.oe2203sp3.aarch64` 包内的 `aarch64-linux-gnu-gcc-10.3.1` 入口点同样为 `0x404440`，与节点上查到的值一致。

## 2.6 [5';10'] 灵晟 LX2 每 NUMA 核心数与单 NUMA FP64 理论算力

**答案**

- 每 NUMA 配置核心数：`38`
- 单 NUMA FP64 理论算力：`7537.5` GFLOPS

**思路**：公开资料查证题（小交问答），无需上集群。

### 实际操作过程

1. 检索关键词「灵晟 LX2」/「LineShine LX2」，核对公开资料：
   - 维基百科「灵晟」：每个计算芯片（die）包含 4 个 NUMA 域，每域 38 个核心；单颗 LX2 共 304 核（2 die × 152）。
   - Dongarra 报告《Report on the Chinese LineShine System》、ServeTheHome、arXiv:2605.08633 均给出：304 核分为 8 个 38 核 cluster，FP64 理论峰值 60.3 TFLOPS。
2. 计算：
   - 每 NUMA 核心数：304 ÷ 8 = **38**（2 die × 4 NUMA = 8 个 NUMA 域）。
   - 单 NUMA FP64：60.3 TFLOPS ÷ 8 = 7.5375 TFLOPS = **7537.5 GFLOPS**。

### 输出解析 / 注意

- 60.3 TFLOPS 是官方四舍五入值；若按 128 FLOP/cycle/核 × 1.55 GHz × 38 核精确计算约为 7539.2 GFLOPS。按题目"单颗 LX2 FP64 峰值均分到 8 个 NUMA"的常规算法，填 `7537.5`。
- 资料出处：维基百科「灵晟」（中/英）、Dongarra 报告 ICL-UT-26-01、ServeTheHome、arXiv:2605.08633。

## 2.7 [10'*2] DeepSeek-V4-Flash-0731：layer 4 sparse attention 最小时间与全层主注意力 KV cache

**答案**

- 第 4 层 sparse attention 理论最小计算时间：`0.559` μs
- 保存所有层主注意力 KV cache：`2.71` GiB

**依据来源**：HuggingFace 官方仓库 `deepseek-ai/DeepSeek-V4-Flash-0731` 的 `inference/config.json`、`inference/model.py`、`inference/kernel.py`（与题面"给出链接的提交"对应）。

### 模型关键结构

- 43 个主 Transformer 层（`n_layers=43`），另有 3 个 DSpark/MTP 投机解码层（不在 `model.layers` 内）；`compress_ratios` 共 46 项 = 43 主层 + 3 个 MTP 层（`DSparkAttention` 断言 `compress_ratio == 0`，对应末尾三个 0）。
- 逐层 `compress_ratios`（主层）：第 1、2 层为 0；其后奇偶交替 **4 / 128**（ratio 4 共 21 层、ratio 128 共 20 层）。
- `head_dim=512`、`n_heads=64`、`window_size=128`、`index_topk=512`；仅 `compress_ratio==4` 的层创建 `Indexer`。
- `Attention.kv_cache` 缓冲区：`[max_batch_size, window_size + max_seq_len // compress_ratio, head_dim]`，默认 dtype BF16（2 B/元素）。
- `sparse_attn`（kernel.py）：按 `topk_idxs` gather KV → 两次 BF16 GEMM（QKᵀ 与 PV）→ 片上 online softmax；`attn_sink` 只给 softmax 分母加 `exp(sink)`，不构成矩阵乘。

### 第一空：第 4 层（compress_ratios[3] = 128）sparse attention

decode 时 `start_pos=524287, seqlen=1, bsz=1`，ratio=128 层无 Indexer，`get_compress_topk_idxs` 返回**全部**压缩位置：

- 参与注意力的 KV 数 = window 128 + 524288/128 = 4096 → **topk = 4224**，`h=64, d=512`，全 BF16。

**FLOPs**（仅计两次矩阵乘，FMA=2 FLOPs）：

```text
2 × (2 × 64 × 4224 × 512) = 553,648,128 FLOPs
```

**HBM 流量**（每份数据仅访问一次，中间结果留在片上；四个输入张量初始均驻留 HBM）：

| 张量 | 形状 | dtype | 字节 |
| --- | --- | --- | ---: |
| q | [1,1,64,512] | BF16 | 65,536 |
| kv（gather 全部 4224 行） | [1,4224,512] | BF16 | 4,325,376 |
| attn_sink | [64] | FP32 | 256 |
| topk_idxs | [1,1,4224] | INT32 | 16,896 |
| o（写回） | [1,1,64,512] | BF16 | 65,536 |
| **合计** | | | **4,473,600** |

**B200 SXM 官方规格（SI 单位）**：BF16 Tensor Core dense 2.25 PFLOPS = 2.25×10¹⁵ FLOP/s；HBM3e 带宽 8 TB/s = 8×10¹² B/s。

- t_compute = 5.53648128×10⁸ / 2.25×10¹⁵ = 0.2461 μs
- t_memory = 4.4736×10⁶ / 8×10¹² = 0.5592 μs
- 计算/访存完全重叠取 max：**0.559 μs**（访存受限：计算强度 123.8 FLOP/B < 机器平衡点 281.25 FLOP/B）

注：若不计 attn_sink 与 topk_idxs 的读取流量则为 0.557 μs；按题面"必要输入张量……输入均已生成且初始仅驻留 HBM"，四个输入都应计入，取 0.559。

### 第二空：所有主层 attn.kv_cache 总量（ctx = 524288，BF16）

每层缓存 token 数 = `window(128) + 524288/ratio`（ratio=0 层仅 128），每 token 512×2 B：

| ratio | 层数 | 每层 token 数 | 每层字节 | 小计（B） |
| --- | --- | --- | --- | ---: |
| 0 | 2 | 128 | 131,072 | 262,144 |
| 4 | 21 | 131,200 | 134,348,800 | 2,821,324,800 |
| 128 | 20 | 4,224 | 4,325,376 | 86,507,520 |
| | | | **总计** | **2,908,094,464** |

```text
2,908,094,464 B / 2^30 = 2.7084 GiB → 2.71 GiB（3 位有效数字）
```

注：按题面 tip 仅统计 `model.layers` 中各层 `attn.kv_cache` 张量，排除 3 个 MTP（DSpark）层的缓存、Indexer 自身的 kv_cache（[1, 131072, 128] BF16）以及 Compressor 的 kv_state/score_state 状态缓冲。
