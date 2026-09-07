# POD5 数据保存与压缩说明

本文梳理当前仓库如何把纳米孔测序信号写入 `.pod5` 文件，以及 VBZ 压缩的实现路径。代码以 C++ 核心库 `c++/pod5_format` 为准，C API 与 Python 绑定都走同一套逻辑。

先读第 1.1 节的体积结论，第 2.1 节一次 Run 如何拆成多个文件，再读第 5 节的 VBZ 逐步算法与例子。

---

## 1. 项目在存什么

POD5 是 Oxford Nanopore 的测序原始数据格式，用来替代 FAST5。一份文件里同时存：

| 内容 | 物理类型 | 说明 |
|------|----------|------|
| 原始信号 | `int16` ADC 采样点 | 每个通道每个时刻一个整数；物理电流（pA）由标定换算 |
| Read 元数据 | Arrow 列 | read_id、通道、well、起止、标定、结束原因等 |
| Run Info | Arrow 列 | 一次采集/实验的公共信息（采样率、flow cell、试剂盒等） |

标定公式（读取侧）：

```
pA = (ADC + calibration_offset) * calibration_scale
```

默认写入的是 ADC，不是 pA。

### 1.1 结论：体积几乎全是信号，省空间的核心是 VBZ

按当前机器的真实工况估算（下文 GB/TB 均按 \(10^9\) / \(10^{12}\) 字节）：

| 参数 | 取值 |
|------|------|
| 采样率 | **5000 Hz** |
| 每个采样点 | `int16`，**2 字节** |
| 一次 Run | **48 小时** \(= 1.728 \times 10^5\) 秒 |
| 产出 | 约 **3200 万条 Reads** |

**先看单通道、单条 read，量级就已经差开了。**

| 对象 | 计算 | 体积 |
|------|------|------|
| 1 秒信号 | \(5000 \times 2\) | **10 KB** |
| 1 条 10 秒的 read（偏短） | \(5\times10^4 \times 2\) | **100 KB** |
| 1 条 2 分钟的 read | \(6\times10^5 \times 2\) | **1.2 MB** |
| Reads 表一行元数据 | uuid、通道、标定、结束原因、signal 行号列表等 | **约 200～300 字节** |
| 单通道连续采满 48 h | \(5000 \times 48 \times 3600 \times 2 = 1.728\times10^9\) | **1.73 GB** |
| Run Info | 整个 Run 通常 1 行 | **几 KB～几十 KB** |

一条 10 秒的 read：100 KB 信号 vs 约 250 字节元数据，信号大约是元数据的 **400 倍**。更长的 read 这个倍数还要大。

**再看一整次 48 h Run、3200 万条 Reads。**

Reads 表按每行 250 字节计（含 Arrow 编码余量）：

\[
3.2\times10^7 \times 250 \approx 8\,\text{GB}
\]

这已经不是「几 KB」了——read 数量上来以后，元数据本身也有数 GB。但和信号比仍然很小。信号体积取决于 **全部 read 的采样点总数**，也就是平均每条 read 活了多久：

\[
\text{未压缩信号} = 3.2\times10^7 \times T_{\text{avg}} \times 5000 \times 2
= 3.2\times10^{11} \times T_{\text{avg}}\;\text{字节}
= 320\,\text{GB} \times T_{\text{avg}}/\text{秒}
\]

| 平均每条 read 时长 \(T_{\text{avg}}\) | 未压缩信号 | 相对 8 GB 元数据 | 含义 |
|--------------------------------------|------------|------------------|------|
| 2 秒（大量短 read / 频繁 unblock） | **0.64 TB** | 约 **80 倍** | 偏保守的下限 |
| 10 秒 | **3.2 TB** | 约 **400 倍** | 中等、很常见 |
| 20 秒 | **6.4 TB** | 约 **800 倍** | 更长的基因组 read 混进平均 |

48 h 墙钟、3200 万条 read，相当于全芯片合计约 **185 条 read/秒** 在收尾。只要平均每条 read 有几秒有效信号（纳米孔测序的常态），**一次 Run 的未压缩 ADC 就是 0.6～数 TB**；Run Info 仍可忽略，Reads 表大约 8 GB，占比通常 **不到 2%**。文件体积几乎等于信号。

POD5 把 Run Info 抽成单独一张表、pore/end_reason 用字典，主要是避免像 FAST5 那样 **每条 read 重复拷贝同一套实验信息**。在 3200 万行这个规模上，这能省掉不少重复字符串，也让写更快，但相对 TB 级信号，不是主力。

**真正把体积压下来的，是对信号做 VBZ。** 默认 `SignalType::VbzSignal`。不做 VBZ 时，Signal 列就是 `LargeList<int16>`，按点原样存，**2 字节/点**，一次 Run 就要按上表的 0.64～6.4 TB 落盘。VBZ 专门吃纳米孔这种平滑 ADC 时间序列，常见能把原始 `int16` 压到大约 **1/3～1/5**（具体看信号平稳程度）。按 4:1 粗算：

| 平均 read 时长 | 未压缩信号 | VBZ 后约 | 一次 Run 少存 |
|----------------|------------|----------|----------------|
| 2 秒 | 0.64 TB | **0.16 TB** | 约 **0.5 TB** |
| 10 秒 | 3.2 TB | **0.8 TB** | 约 **2.4 TB** |
| 20 秒 | 6.4 TB | **1.6 TB** | 约 **4.8 TB** |

这才是「纳米孔原始数据还能存得下、盘阵不会被一次 Run 打满」的原因。元数据再怎么优化，也挤不出这几 TB。

需要分清：

- **VBZ 不是 POD5 发明的独门格式**，FAST5/MinKNOW 里就已经在用同类思路；POD5 的贡献是：默认就压信号、容器更干净（Arrow + 三张表）、切块可读、流式可恢复。没有 VBZ，POD5 仍然比「每条 read 一份 HDF5 元数据」省一点，但 **省不出数量级**。
- **切块（102400 点，5000 Hz 下约 20.5 秒一块）不省空间**，是为了长 read 可以只解一段。字典列、Arrow 列式布局主要优化元数据和访问，不是信号本体。

一句话：**一次 48 h、5000 Hz、3200 万 read 的 Run，未压缩信号是 TB 级，Reads/Run Info 合计大约数 GB；POD5 能把测序数据存得这么紧，核心就是对信号做 VBZ。** 算法逐步说明见第 5 节。

---

## 2. 文件长什么样

`.pod5` 不是单张 Arrow 表，而是一个容器：里面嵌了 **三份 Apache Feather V2（Arrow IPC File）**，用签名和 footer 包起来。

```
┌─────────────────────────────────────────────┐
│ 签名 8 字节: \213POD\r\n\032\n              │
│ section marker: 16 字节 UUID（本文件随机）   │
├─────────────────────────────────────────────┤
│ 嵌入文件 1: Signal Table  (Feather V2)      │  ← 直接写在主文件里
│ 8 字节对齐填充 + section marker             │
├─────────────────────────────────────────────┤
│ 嵌入文件 2: Run Info Table (Feather V2)     │  ← close 时从临时文件拼进来
│ 8 字节对齐填充 + section marker             │
├─────────────────────────────────────────────┤
│ 嵌入文件 3: Reads Table    (Feather V2)     │  ← close 时从临时文件拼进来
│ 8 字节对齐填充 + section marker             │
├─────────────────────────────────────────────┤
│ "FOOTER\0\0"                                │
│ FlatBuffers Footer（记录三张表的 offset/len）│
│ footer 长度 (int64 LE)                      │
│ section marker                              │
│ 签名 8 字节（与文件头相同）                  │
└─────────────────────────────────────────────┘
```

实现见 `c++/pod5_format/internal/combined_file_utils.h`。

设计要点：

- **顺序追加**：仪器可以边测边写。
- **可恢复**：写崩时临时文件里的 Arrow 表仍可扫描；完整文件以尾部签名 + footer 判定。
- **可 mmap**：Arrow 列式布局，读路径不必锁文件。

三张表的 schema metadata 里都带同一组键：`MINKNOW:pod5_version`、`MINKNOW:software`、`MINKNOW:file_identifier`。footer 里的 `file_identifier` 必须与之一致。

### 2.1 一次 Run 为何会有几十个 .pod5（MinKNOW 如何切文件）

**POD5 格式本身不规定「一个 Run 几个文件」。** 一个 `.pod5` 只是一份可流式写入的容器。仪器上一次 Run 拆成很多个文件，是 **MinKNOW 的 batching（分批落盘）**，不是按通道、也不是本仓库 C++ 库自动切的。

官方默认（ONT Output Specifications：
[POD5 Read batching](https://software-docs.nanoporetech.com/output-specifications/latest/read_formats/pod5/)、
[Batching](https://software-docs.nanoporetech.com/output-specifications/latest/minknow/batching/)）：

| 条件 | 默认值 | 含义 |
|------|--------|------|
| `batch_duration` | **3600 秒（1 小时）** | 距上一份文件关闭已过 1 小时，且至少写过 1 条 read，就开新文件 |
| `bases_per_batch` | **5 亿 bases** | 这份里预估碱基到了 500 Mbases，也开新文件 |
| 实际切分 | **两者谁先到** | 产量高时可能不到 1 小时就切；产量低时按时切 |

文件名一般是：

```text
{flow_cell_id}_{short_protocol_run_id}_{short_run_id}_{batch_number}.pod5
```

末尾的 `batch_number` 是第几批（0, 1, 2, …），**不是通道号**。配置在 MinKNOW 安装目录的 `conf/package/shared/default_writer.toml` 里 `[writer_configuration.read_pod5]`。也可以改成只按 read 数（例如旧 FAST5 常见的 4000 条/文件）、只按小时、或只按碱基数。

**一个 pod5 里是什么**

- 这一批时间/产量窗口内、**已经结束并写入** 的 reads（**多通道混在一起**）。
- 同一条很长的 read 只会出现在 **它结束时** 正在写的那个文件里，不会按采样时刻拆到两个文件。
- 目录上还可能有 `pod5/` 和 `pod5_skip/`（跳过碱基识别等），那是按处理状态分目录，不是按时间。

**和 ONT 测试数据对得上：** 一次 Run 约 47 个文件、每个大约 130～170 MB。

- 约 **48 小时** 的 Run，若主要按 **1 小时** 切，会得到大约 **47～48 个** 文件；最后一份往往不满整小时，体积略小。
- 各文件 **130～170 MB 比较齐**，说明每个时间窗里的数据量差不多（通量较稳），更像按时切，而不是按固定 4000 条 read（那种会出几千个小文件）。
- 所以可以把它理解成：**大约每小时（或先达到 5 亿 bases）的一段测序产出**，不是整次 Run 的一个连续原始波形，也不是按孔/通道各一个文件。

核实方法：看文件名里的 `batch_number`；再用 `pod5 inspect` 或各文件里 read 的 `start` / 采集起始时间，看相邻文件窗口是否大约差 1 小时。

---

## 3. 三张表如何互相引用

```
Reads 表（一行 = 一条 read）
  read_id  ─────────────────────────────────┐
  signal: list<uint64>  ──► Signal 表行号   │ 必须同一 read_id、按时间顺序
  run_info: dict index  ──► Run Info 表     │
  pore_type / end_reason: 字典列            │
  num_samples: 该 read 全部采样点数         │
                                            ▼
Signal 表（一行 = 一段 chunk）
  read_id
  signal     ← 未压缩: LargeList<int16>
             ← VBZ:    minknow.vbz (LargeBinary)
  samples    ← 本行采样点数（解压 VBZ 必须知道）
```

一条长 read 会被切成多行 Signal。Reads 表的 `signal` 列保存这些行号，`num_samples` 等于各 chunk `samples` 之和。

默认切块大小：

```c++
// file_writer.h
DEFAULT_SIGNAL_CHUNK_SIZE = 102'400;   // 每个 Signal 行最多 102400 个 int16
DEFAULT_SIGNAL_TYPE       = VbzSignal; // 默认压缩
DEFAULT_SIGNAL_TABLE_BATCH_SIZE = 100;
DEFAULT_READ_TABLE_BATCH_SIZE   = 1000;
```

切块原因：长 read 不必一次全解压；训练/抽片段可以只解需要的 chunk。

---

## 4. 写入时序（保存数据）

### 4.1 打开文件

`pod5_create_file(path, writer_name, options)` → `create_file_writer()`：

1. 生成两个 UUID：`file_identifier`、`section_marker`。
2. 在目标路径打开主文件，先写签名 + section marker，然后 **Signal 表直接写在主文件后半段**。
3. Reads / Run Info 先写到旁边的临时文件：
   - `.{file_identifier}.tmp-run-info`
   - Reads 对应的 tmp 路径
4. 建三套 Arrow RecordBatchWriter。

关闭 `pod5_close_and_free_writer()` 时：把两份临时表按 8 字节对齐追加进主文件，写 FlatBuffers footer，删临时文件。

### 4.2 必须先登记字典数据

写入 read 之前：

```
pod5_add_pore(...)       // pore_type 字典
pod5_add_run_info(...)   // Run Info 表 + Reads 表里的 run_info 字典
```

`end_reason` 是内置字典（unknown / mux_change / signal_positive 等）。缺少 run_info 或 pore 会返回 `POD5_ERROR_INVALID`。

### 4.3 两条写信号路径

**路径 A：库内压缩（常用）**

```
pod5_add_reads_data(file, n, VERSION_3, &row_data, signal_ptr[], signal_size[])
    → FileWriter::add_complete_read(read_data, int16_span)
        → 按 max_signal_chunk_size 切块
        → SignalTableWriter::add_signal()
            → visitors::append_signal
                → 若 VbzSignal: compress_signal()
                → 若 Uncompressed: 原样写入 LargeList<int16>
        → ReadTableWriter::add_read(read_data, signal_row_ids, total_samples)
```

调用方只给原始 `int16*`。切块、压缩、登记行号都在库里完成。

**路径 B：调用方预压缩**

```
pod5_vbz_compress_signal(int16, n, buf, &size)     // 先压成 VBZ 字节
pod5_add_reads_data_pre_compressed(
    file, n, VERSION_3, &row_data,
    compressed_chunks[][], chunk_sizes[][],
    sample_counts[][], chunk_count[])
    → 每个 chunk: add_pre_compressed_signal()  （不再二次压缩）
    → add_complete_read(read_data, signal_row_ids, total_samples)
```

适合已经在别的线程/流水线里压好、或要自己控制 chunk 边界的场景。`[mytest1]` 第二条 read、`[mytest3]` 都走这条路径。

注意：预压缩路径 **不会再按 102400 切块**。一整段 VBZ 会写成 Signal 表的一行。超大数组应自己切 chunk 再传入。

### 4.4 写入选项（C API）

```c
enum CompressionOption {
    DEFAULT_SIGNAL_COMPRESSION = 0,  // 等同 VBZ
    VBZ_SIGNAL_COMPRESSION     = 1,
    UNCOMPRESSED_SIGNAL        = 2,
};

struct Pod5WriterOptions {
    uint32_t max_signal_chunk_size;      // 0 = 102400
    int8_t   signal_compression_type;    // 见上
    size_t   signal_table_batch_size;    // 0 = 100
    size_t   read_table_batch_size;      // 0 = 1000
};
```

`pod5_create_file(..., NULL)` 即默认 VBZ。整文件只能选一种 signal 类型，不能混用。

### 4.5 读回

```
pod5_open_file
pod5_get_read_count / pod5_get_read_ids
pod5_get_read_batch → pod5_get_read_batch_row_info_data
pod5_get_read_complete_signal   // 按 Reads.signal 行号取出各 chunk，VBZ 则解压后拼接
```

解压在 `SignalTableRecordBatch::extract_signal_row()`：看 schema 是 `LargeList<int16>` 还是 `minknow.vbz`。

---

## 5. 压缩：VBZ 算法逐步说明

VBZ = **V**ariant **B**yte + **Z**std。只作用于 Signal 列，不压 Reads / Run Info。实现：`c++/pod5_format/signal_compression.cpp` + `c++/pod5_format/svb16/`。POD5 里固定打开两项开关：

```c++
static constexpr bool UseDelta = true;
static constexpr bool UseZigzag = true;
```

也就是：**先差分，再 zigzag，再 StreamVByte-16 变长打包，最后 zstd level 1。**

### 5.1 对输入数据的要求

| 约束 | 说明 |
|------|------|
| 类型 | 必须是 `int16_t` 数组（`SampleType`）。这是 ADC 原始值，不是 pA 浮点。 |
| 字节序 | 主机内存中的 `int16`；SVB 把「需要 2 字节」的值按 **little-endian** 写入 data 区。 |
| 长度 N | \(1 \le N \le 2^{32}-1\)。N=0 合法但不写有效载荷。超过 `uint32` 上限会报错（SVB 用 32 位计数）。默认写入还会按 102400 点切块，单块远小于上限。 |
| 取值范围 | 算法对全范围 `int16`（-32768～32767）都正确。纳米孔 ADC 通常更窄（Run Info 里 `adc_min`/`adc_max`，常见约 -4096～4095），差值更小，压缩更好。 |
| 统计特性（压缩效果，不是正确性） | **相邻点变化要小。** 这是纳米孔电流的常态。若输入是白噪声或乱序，delta 帮不上忙，很多值会落到 2 字节，VBZ 收益变差，zstd 也救不回数量级。 |
| 解压必须另传 N | svb16 流 **不内嵌样本个数**。读端用 Signal 表的 `samples` 列当 N，用来算 key 区长度、校验 data 区、分配输出缓冲。N 传错会解失败或解出垃圾。 |
| 预压缩写入 | `pod5_vbz_compress_signal` 的输出必须原样交给 `pod5_add_reads_data_pre_compressed`，并带上正确的 `sample_counts`。库不会再压一次，也不会替你切 102400 块。 |
| 整文件一致 | 一个 `.pod5` 的 Signal 列要么全是 VBZ，要么全是未压缩 `LargeList<int16>`，不能混用。 |

适配 VBZ 的典型输入：一条通道上按时间排列的 ADC，例如 `[..., 120, 121, 119, 118, 400, 401, ...]`（平稳 + 偶发跳变）。不适合：已经换成 pA 的 `float`、交错多通道、或随意打乱的数组。

### 5.2 总流水线

```
int16 samples[N]                         // 输入：ADC
        │
        │  步骤 A  Delta
        │     d[0] = x[0] - 0
        │     d[i] = x[i] - x[i-1]      // uint16 环绕减法，保留负差
        │
        │  步骤 B  Zigzag
        │     把有符号差值映到非负整数：0,-1,1,-2,2,... → 0,1,2,3,4,...
        │
        │  步骤 C  StreamVByte-16
        │     每个数：< 256 写 1 字节，否则写 2 字节（LE）
        │     每 8 个数一个 key 字节，对应 bit=1 表示该数用了 2 字节
        ▼
  svb16 字节流 = [key 区][data 区]
  key 区长度 = ceil(N / 8)
  data 区最长 = 2N
        │
        │  步骤 D  ZSTD_compress(..., level = 1)
        ▼
  VBZ 字节流  → Arrow 列 minknow.vbz（物理类型 LargeBinary）
```

解压严格反向：`ZSTD_decompress` → 用 N 算出 key 长度并 `svb16::validate` → `decode`（先按 key 取出变长整数，再 zigzag 还原差值，再累加还原样本）。x86 上 encode 优先 SSSE3、decode 优先 SSE4.1，否则走 `encode_scalar` / `decode_scalar`。

### 5.3 步骤 A：Delta（差分）

源码（`encode_scalar.hpp`）：

```c++
value = uint16_t(in[c]) - uint16_t(prev);  // 无符号环绕
prev  = in[c];
```

第一个点的 `prev = 0`，所以 `d[0] == x[0]`（按 uint16 解释）。必须在 **uint16** 里减，这样 `11 - 12` 得到 `65535`，正好是 `-1` 的二进制，后面 zigzag 才能还原。

纳米孔信号相邻点常常只差几个 ADC，这一步把「绝对值几百～几千」变成「差值 0～几十」。

### 5.4 步骤 B：Zigzag

差分后有正有负。直接变长编码的话，`-1` 作为 uint16 是 `0xFFFF`，会被当成「必须 2 字节的大数」，把 delta 的好处全吃掉。Zigzag 把靠近 0 的负数也映成小的正整数：

```
zigzag_encode(v) = (v << 1) ^ (int16(v) >> 15)   // 实现里写成 (v+v) ^ (int16(v)>>15)
zigzag_decode(u) = (u >> 1) ^ (0 - (u & 1))
```

对照表（对 **差值** 而言）：

| 有符号差值 | zigzag 后（无符号） | 能否 1 字节 |
|------------|---------------------|-------------|
| 0 | 0 | 是 |
| -1 | 1 | 是 |
| 1 | 2 | 是 |
| -2 | 3 | 是 |
| 2 | 4 | 是 |
| -128 | 255 | 是（刚好 1 字节上限） |
| 128 | 256 | **否，要 2 字节** |
| -129 | 257 | 否 |

因此：**相邻 ADC 变化的绝对值 ≤ 127，该点一定 1 字节；≥ 128 才落到 2 字节。** 开孔、堵孔那种跳变会偶发 2 字节，不影响整体。

### 5.5 步骤 C：StreamVByte-16（变长打包）

布局：

```
[ key_0 | key_1 | ... | key_{ceil(N/8)-1} | data... ]
```

- `key` 区长度：`svb16_key_length(N) = ceil(N / 8)`。
- 每个 key 字节管 8 个样本（最后一字节可能不足 8 个，高位未用的 bit 为 0）。
- key 的 bit `k`（从 bit0 到 bit7，对应该组第 0～7 个数）：
  - `0`：该数在 data 区占 **1** 字节（zigzag 值 0～255）
  - `1`：该数在 data 区占 **2** 字节，little-endian
- data 区按样本顺序紧密排列，没有对齐填充。

最大编码长度：`ceil(N/8) + 2N`（全是 2 字节时）。压缩上界 `pod5_vbz_compressed_signal_max_size` 再对此做 `ZSTD_compressBound`。

### 5.6 步骤 D：zstd level 1

svb16 之后，key 区全是「几乎全 0」的比特图，data 区是一串小整数，仍有冗余。`ZSTD_compress(..., 1)` 再压一层：

- level 1：压缩比够用、CPU 便宜，适合仪器边采边写。
- 输出才是写入 Arrow 的 VBZ blob。
- **很短的数组**（几十字节）zstd 帧头可能比收益大，压缩比看起来一般；默认 102400 点一块时这一层才稳定划算。

解压时先 `ZSTD_getFrameContentSize` 得到 svb16 流长度，校验不超过 `svb16_max_encoded_length(N)`，再解到中间缓冲（x86 还要在尾部留 16 字节 SIMD padding）。

### 5.7 完整例子 1：8 个平稳点（一个 key 字节，全 1 字节）

输入（模拟缓慢漂移的 ADC）：

```
x = [10, 12, 11, 11, 13, 14, 15, 16]
原始体积 = 8 × 2 = 16 字节
```

**Delta**（`prev` 从 0 起）：

| i | x[i] | 计算 | d[i]（有符号） | d[i] 的 uint16 |
|---|------|------|----------------|----------------|
| 0 | 10 | 10-0 | 10 | 10 |
| 1 | 12 | 12-10 | 2 | 2 |
| 2 | 11 | 11-12 | -1 | 65535 |
| 3 | 11 | 11-11 | 0 | 0 |
| 4 | 13 | 13-11 | 2 | 2 |
| 5 | 14 | 14-13 | 1 | 1 |
| 6 | 15 | 15-14 | 1 | 1 |
| 7 | 16 | 16-15 | 1 | 1 |

**Zigzag**：

```
10 → 20
 2 → 4
-1 → 1
 0 → 0
 2 → 4
 1 → 2
 1 → 2
 1 → 2
```

全部 &lt; 256。

**SVB key + data**：

```
key 只有 1 字节，8 个 bit 全 0  →  0x00
data                            →  14 04 01 00 04 02 02 02   (hex)
svb16 流                        →  00 14 04 01 00 04 02 02 02     共 9 字节
```

相对原始 16 字节，仅 svb16 就已经约 **1.8:1**。再过 zstd 对这么短的缓冲收益不稳定（帧头开销），真实测序块是 10 万点量级，zstd 会再削一截。

解压：N=8 → key 长 1 → 读到 `0x00` 知 8 个数都是 1 字节 → 取出 20,4,1,0,4,2,2,2 → zigzag 还原 10,2,-1,0,2,1,1,1 → 累加 `10, 12, 11, 11, 13, 14, 15, 16`。

### 5.8 完整例子 2：夹一次大跳变（出现 2 字节）

输入：

```
x = [100, 101, 500]
原始体积 = 6 字节
```

Delta：`100`，`1`，`399`。

Zigzag：`200`，`2`，`798`。其中 `798 = 0x031E ≥ 256`，要 2 字节。

3 个样本 → 1 个 key 字节，只用低 3 bit（第 0、1 个数 1 字节 → bit0=bit1=0；第 2 个数 2 字节 → bit2=1）：

```
key  = 0b00000100 = 0x04
data = C8 | 02 | 1E 03
       └200  └2   └798 little-endian
svb16 = 04 C8 02 1E 03     共 5 字节
```

跳变点多花 1 字节，前后平稳点仍是 1 字节。这就是「偶发 unblock / mux 跳变不会把整条 read 压爆」的原因。

若输入是毫无相关的噪声，例如差值经常 &gt; 127，key 会变成几乎全 1，data 接近 `2N`，svb16 几乎不缩小，只剩 zstd 对随机数据的微弱收益——所以 **VBZ 的前提是时间上平滑的 ADC**。

### 5.9 例子 3：和测试用例对齐

`[mytest2]` 用 `iota` 生成 20 个点：`-10, -9, ..., 9`。相邻差恒为 +1，zigzag 后几乎全是 `2`，svb16 的 data 区会非常规整，再叠加 zstd，压缩比会明显高于「按 int16 原样存储」。`[mytest3]` 对真实 `reads_all.dat` 做同一套预压缩，打印的 ratio / space savings 就是「原始 2N 字节 vs VBZ blob」的对比（还没算 Arrow/容器开销；信号占绝对大头时，这个比值约等于整文件的节省）。

### 5.10 在 Arrow 里怎么表示

```c++
// types.h
class VbzSignalType : public arrow::ExtensionType {
    // extension_name = "minknow.vbz"
    // 物理存储 = arrow::large_binary()
};
```

未压缩时同一列是 `large_list(int16)`。读端不认识 extension 就读不了信号，但 Reads / Run Info 仍可用通用 Arrow 工具看。

Reads 表里 pore_type、end_reason、run_info 用 **Arrow dictionary**，重复字符串只存一份。这是元数据层的空间优化，与 VBZ 无关。

---

## 6. 调用栈对照（方便改代码）

| 你想做的事 | C API | C++ |
|------------|-------|-----|
| 建文件 | `pod5_create_file` | `create_file_writer` |
| 加 pore / run | `pod5_add_pore` / `pod5_add_run_info` | `FileWriter::add_pore_type` / `add_run_info` |
| 写未压缩 int16（库内 VBZ） | `pod5_add_reads_data` | `add_complete_read(read, int16_span)` |
| 自己 VBZ 再写入 | `pod5_vbz_compress_signal` + `pod5_add_reads_data_pre_compressed` | `compress_signal` + `add_pre_compressed_signal` |
| 关文件 | `pod5_close_and_free_writer` | `FileWriter::close` |
| 读完整信号 | `pod5_get_read_complete_signal` | `FileReader::extract_samples` |
| 只压/解内存 | `pod5_vbz_compress_signal` / `decompress` | `compress_signal` / `decompress_signal` |

---

## 7. 仓库里现有测试如何对应

`c++/test/c_api_tests.cpp`：

| Tag | 在测什么 |
|-----|----------|
| `[mytest1]` `SCENARIO("C API Reads")` | 建文件 → 写 pore/run_info → 第一条 read 用 `pod5_add_reads_data`（库内压缩）→ 第二条用 VBZ 预压缩 + `pod5_add_reads_data_pre_compressed` → 读回校验 |
| `[mytest2]` `TEST_CASE("VBZ compression")` | 不落盘，纯内存压/解 20 个 int16，并测错误缓冲区、超大 sample 数 |
| `[mytest3]` | 读 `./test_data/reads_all.dat`（裸 `int16` 二进制）→ 预压缩 → 写入 `./test_data/output_signal.pod5` → 打印压缩比 → 再读回比对 |

跑法：

```bash
export LD_LIBRARY_PATH=~/.conda/envs/pod5-deps/lib:${LD_LIBRARY_PATH:-}
./build/c++/test/pod5_unit_tests "[mytest1]"
./build/c++/test/pod5_unit_tests "[mytest2]"
./build/c++/test/pod5_unit_tests "[mytest3]"
```

---

## 8. 一张图串起来

```
用户 int16[]                          用户已压好的 VBZ bytes
      │                                        │
      │ pod5_add_reads_data                    │ pod5_add_reads_data_pre_compressed
      ▼                                        ▼
  按 102400 切块                          每个 chunk 一行（不再切）
      │                                        │
      └──────────┬─────────────────────────────┘
                 ▼
        SignalTableWriter
                 │
     ┌───────────┴───────────┐
     │ 默认 VbzSignal        │ 可选 Uncompressed
     │ svb16(delta+zigzag)   │ LargeList<int16>
     │ + zstd level 1        │
     └───────────┬───────────┘
                 ▼
     Signal 表 RecordBatch（写进主 .pod5）
                 │
     Reads 表记下 signal 行号 + num_samples（先写 tmp）
     Run Info 表（先写 tmp）
                 │
              close()
                 ▼
     主文件追加 Run Info、Reads，写 FlatBuffers footer
                 ▼
              foo.pod5
```

---

## 9. 相关源文件

| 路径 | 职责 |
|------|------|
| `docs/SPECIFICATION.md` | 容器布局、表字段规格 |
| ONT [POD5 / Batching](https://software-docs.nanoporetech.com/output-specifications/latest/read_formats/pod5/) | MinKNOW 如何把一次 Run 拆成多个 `.pod5` |
| `docs/tables/{reads,signal,run_info}.toml` | 各列类型与含义 |
| `c++/pod5_format/file_writer.{h,cpp}` | 建文件、切块、拼容器 |
| `c++/pod5_format/signal_table_writer.cpp` | Signal 行写入 / batch |
| `c++/pod5_format/signal_builder.h` | 未压缩 vs VBZ 列构建 |
| `c++/pod5_format/signal_compression.cpp` | VBZ 压/解 |
| `c++/pod5_format/svb16/` | StreamVByte-16 |
| `c++/pod5_format/c_api.{h,cpp}` | C 接口 |
| `c++/pod5_format/internal/combined_file_utils.h` | 签名、footer、嵌入文件 |
| `c++/pod5_format/signal_table_reader.cpp` | 读信号并按类型解压 |
