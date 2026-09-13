# [TH 12.3-Tool] Xml2Bin
$$警告：此项目完全由 AI 生成（除了README.md）$$

这是一个把原为**XML文件**的动画标记文件转换成占用更小的**二进制文件**的工具

### 可支持的启动项：
| 启动项 | 描述 |
| --- | --- |
| `-InputPath <path>` | 提供 .xml 文件，或者提供整个文件夹的路径（看情况搭配`-Recurse`启动项） |
| `-Recurse` | 自动搜索`-InputPath`指定文件夹路径下的所有子目录 |
| `-Verify` | 处理完当前文件的所有`段`的时候，自动校验Bin文件 |
| `-CombineSprite <folder_path>` | 根据指定文件夹路径下的所有图片文件，生成一个Bin文件（动画名称默认为default） |
| `-Extension <ext_name>` | 可自定义扩展名，不带此启动项默认为`.bmp`，留空字符串则不添加扩展名 |
| `-ParticleSuffix <suffix>`| 自定义粒子效果Bin文件的后缀（默认为_p） |
| `-LayoutSuffix <suffix>`| 自定义界面布局txt文件的后缀（默认为_layout） |
| `-Strict`| 出现警告即报错（默认仅警告并跳过） |

### 输出文件：
一般来说，一个Xml文件只会输出一个与之对应的Bin文件，但是在有粒子效果和Layout（界面布局）的时候会单独分离出来
- `*.bin`    ← 角色/物体动画文件
- `*_p.bin`  ← 弹幕/粒子效果动画文件
- `*_layout.txt` ← 界面布局文件

> 注：Layout 文件仅在xml内有 `layout` 字段才能生成

### Binary 格式小端序：
    Header(32B): "SKMP"/"SKAP"/"SKSP", u16 version=1, u16 headerSize=32,
                 u32 stringCount, u32 stringTableOffset(=32), u32 cloneCount,
                 u32 recordCount, u32 totalFrames, u32 dataOffset

    StringTable: u32 count, 每项 u16 len + UTF8 字节
    CloneTable : (i32 id, i32 target) *
    RecordTable: (i32 id, u8 index, u8 loop, u8 movelock, u8 actionlock,
                  u32 frameCount, Frame*) *

    Frames     : u32 imageIdx, u16 index, i32 xtexoffset, i32 ytexoffset,
                 u16 texwidth, u16 texheight, i32 xoffset, i32 yoffset,
                 u16 duration, u8 unknown, u8 rendergroup,
                 u16 blendCount     + Blend(15B)*
                 u16 attackCount    + (u16 boxCount + Box5(10B)*)*
                 u16 collisionCount + (u16 boxCount + Box4(8B)*)*
                 u16 hitCount       + (u16 boxCount + Box4(8B)*)*
                 u16 effectCount    + (i32 x9)*
                 u16 traitsCount    + (i32 x20 + u16 flagCount + u16 flagIdx*)*

注：
- `SKMP` 为角色/物体Binary文件魔数
`SKAP` 为粒子效果Binary文件魔数
`SKSP` 为使用`-CombineSprite`的Binary文件魔数

- `SKSP`（`-CombineSprite` 精灵合并）结构同 `SKAP`，仅魔数不同：
   - 头部 **cloneCount** 槽位 = 动画名在字符串表中的索引（第 0 项即 "default"）

### 杂项：
1. 通常来说，Bin的大小应约为源文件的 **20%~30%** 大小，过大和过小都不正常
2. 每个 Layout 文件都有相对的说明

(c) 2026 LGC No rights reserved
