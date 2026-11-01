# Unity-EditorPreviewAgilityExt

让 Unity 编辑器使用更新的 preview 版 DirectX 12 Agility SDK（如 1.722）的 d3d12.dll 代理工具。
不修改、不覆盖 Unity 安装目录下的任何原有文件，卸载只需删除代理 dll。

## 原理

将代理 `d3d12.dll` 放到 `Unity.exe` 同目录，利用 DLL 搜索顺序接管加载，然后：

1. 内存补丁 Unity.exe 的 `D3D12SDKVersion` / `D3D12SDKPath` 导出，指向 `.\D3D12Pre\` 下的新 Agility 运行时；
2. 调用 `D3D12EnableExperimentalFeatures` 放行实验性 Shader Model（SM 6.10 等 preview 特性必需）。

未激活时纯转发，与系统默认行为完全等价。

## 前置条件

- **Windows 开发者模式必须开启**（preview 版 Agility SDK 的硬性要求）：
  设置 → 系统 → 开发者选项 → 开发人员模式。
- preview 运行时需要显卡驱动支持对应特性。

## 部署

将 `UnityEditor/` 下的三件套复制到 `<Unity安装目录>\Editor\`（即 Unity.exe 所在目录）：

```
Editor\
├─ d3d12.dll            ← 代理（本工具）
├─ agility.uedtcfg      ← 配置文件
└─ D3D12Pre\
   ├─ D3D12Core.dll        ← 新 Agility 运行时（两个 dll 必须同一版本）
   └─ d3d12SDKLayers.dll
```

不碰 Unity 自带的 `D3D12\` 目录与任何文件。

## 配置

`agility.uedtcfg`：

```ini
[agility]
; 1 = 本机所有项目默认走 D3D12Pre 分发
; 0 = 纯转发，仅命令行带 -d3d12pre 的启动走 preview（推荐，按项目分流）
enabled = 1
; 1 = D3D12Pre 部署异常时让设备创建直接失败，暴露配置错误；0 = 静默回退原厂
strict = 0
```

**按项目启用**（推荐）：`enabled = 0`，然后在 Unity Hub → 项目右侧 ⋯ → 添加命令行参数 `-d3d12pre`。命令行优先级高于配置。

## 验证

用 DebugView（以管理员运行，勾选 Capture Global Win32）过滤 `[d3d12proxy]`，正常应看到：

```
[d3d12proxy] probe: core=722(1) layers=722(1) -> OK
[d3d12proxy] patched D3D12SDKVersion: 618 -> 722
[d3d12proxy] preview distribution activated: version=722 path=.\D3D12Pre\
[d3d12proxy] enableExpSM: D3D12EnableExperimentalFeatures -> 0x00000000
```

日志中 `d3d12: failed to query info queue interface (0x80004002)` 为 Unity 自带输出，未装代理时也会出现，可忽略。

## 自行编译

需要 MSVC（v143+，build.bat 中按本机 VS 路径调整 `vcvars64.bat`）：

```
src\build.bat [Unity编辑器目录]
```

构建时会从**本机 System32 的 d3d12.dll** 自动生成导出转发表，因此 Windows 大版本更新后重跑一次 build.bat 即可重新适配，无需改代码。传入编辑器目录时可额外校验 Unity.dll 的 d3d12 导入覆盖情况。

## 已知限制

- 代理必须与 Unity.exe 同目录，否则拒绝激活（防止相对路径解析错误）。
- `D3D12Pre\` 内两个 dll 版本不一致时：strict=0 回退原厂；strict=1 故意让设备创建失败（0x887e0003）以暴露问题。
- preview 特性的实际可用范围仍受驱动限制（如 SM6.10 的 groupshared 扩展上限取决于驱动，实测部分驱动仍为 32KB）。
- 着色器侧的 SM6.0+ 编译支持需配合 [Unity-NextUnityShader](https://github.com/RicciFloOow/Unity-NextUnityShader)。
