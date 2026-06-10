# 将 Flutter Widget / Element / RenderObject 迁移到 C++ —— 技术方案

> 目标：把当前用 Dart 实现的三棵树（Widget 配置树、Element 生命周期树、RenderObject 布局绘制树）整体或部分下沉到 C++ 实现。
> 本文基于当前仓库实际代码边界给出可落地的方案、阶段路线与风险评估。

---

## 0. 先决问题：为什么要做、做到什么程度

这一步决定整个方案形态，必须先定调。不同动机对应完全不同的架构：

| 动机 | 真正想要的 | 对应方案形态 |
|---|---|---|
| **性能**（去掉 Dart GC/JIT 抖动、降低 build/layout 开销） | 热路径 C++ 化 | 保留 Dart 写业务，热点（layout/paint/diff）下沉 C++ |
| **去掉 Dart runtime**（更小体积、嵌入式、纯 AOT C++ 产物） | 整套框架 + 业务都不依赖 Dart VM | 全 C++ 框架 + 自定义 DSL/绑定 |
| **多语言前端**（让 C++/Rust/JS 都能驱动同一套渲染） | 框架内核语言中立 | C++ 内核 + 多语言 binding 层 |
| **与现有 C++ 业务深度集成**（游戏引擎/CAD 内嵌 UI） | UI 树能被 C++ 直接持有/操作 | C++ 内核 + 可选 Dart 外壳 |

**关键判断**：如果动机是"性能"，整套迁移大概率是**错误投资**——Flutter 的 build/layout/paint 在 AOT 模式下已经相当快，瓶颈通常在 GPU/光栅化（已是 C++）或业务侧 rebuild 过度。整套迁移收益最大的场景是**去 Dart runtime** 或 **C++ 宿主深度集成**。

> **本方案已锁定目标：去掉 Dart runtime（纯 AOT C++ 产物，不带 Dart VM）。** 这是最激进的一档，下面 §0.A 专门展开它的真实边界。注意：这个动机**直接否决**了"保留 Dart 外壳"的折中方案——只要还想跑 Dart 业务代码，就甩不掉 Dart VM。其他较温和的形态保留在 §8 供对照。

---

## 0.A 动机锁定：「去掉 Dart runtime」到底意味着什么

这一节是全文最重要的现实校准。**"去 Dart runtime" 不是 "把框架 C++ 化"，它大一个数量级。**

### 0.A.1 要脱 Dart 的不止三棵树，是整个栈的四层

```
① 业务代码（你的 app + 所有 pub 依赖）        ← 全是 Dart，必须换语言重写
② framework（widgets/material/cupertino…）   ← ~400 文件 / 20 万行 Dart，重写或转译
③ dart:ui（lib/ui/*.dart 37 个文件）          ← Canvas/Scene/Picture/PlatformDispatcher 的 Dart 封装，全废弃
④ engine native（flow / DisplayList / Skia） ← 语言中立，这一层才是你能留下的
```

前三层**全是 Dart**。原文档只讨论了 ②（三棵树），但去 runtime 必须连 ① 业务逻辑、③ dart:ui 封装一起处理。**真正最痛的是 ①**：用户 app 的所有逻辑、网络、状态管理、以及依赖的每一个 pub 包，都建立在 Dart 之上。去掉 runtime = 这些全部归零重写。

### 0.A.2 标准 embedder API 用不了——它本身就是"为跑 Dart 而生"

`shell/platform/embedder/embedder.h`（那个 164KB 的标准 C API）看起来像"语言中立的 C 接口"，但它的核心入参出卖了一切：

```
2553:  const uint8_t* vm_snapshot_data;              // Dart VM 快照
2569:  const uint8_t* isolate_snapshot_data;         // Dart isolate 快照
2577:  const uint8_t* isolate_snapshot_instructions; // AOT 编译的 Dart 指令
2451:  /// This is a dedicated thread on which the root Dart isolate is serviced.
2894:  FlutterEngineResult FlutterEngineRun(...)      // 核心动作 = 启动 root Dart isolate 跑 dart:ui
```

`FlutterEngineRun` 干的第一件事就是**创建 root Dart isolate 并执行 `dart:ui`**。换句话说，标准 embedder API 在架构上**强制依赖 Dart runtime**，去 runtime 后这层整个不能用。

> 这正是 **skwasm 的做法**：skwasm 没走标准 embedder，它直接在更底层接 flow / DisplayList / Skia（你已经熟悉的 `canvas.cc` / `surface.cc` / `render_context_skia.cc` 那层）。**去 Dart runtime 的人，要做的就是 skwasm 同款的事——绕过 embedder，自己接 flow 层。**

### 0.A.3 唯一能复用的接缝下移到了 flow / DisplayList 层

原文档说接缝在 `SceneBuilder`（dart:ui）。**那是错的前提下的结论**——`SceneBuilder` 本身是 Dart 类（`compositing.dart`），它的 C++ peer（`lib/ui/compositing/*.cc`）只是 `Dart_NativeFunction` 绑定。去 runtime 后这些 binding 全废。

真正语言中立、可直接 C++ 调用的边界是 **再往下一层**：

| 你以为的接缝（其实是 Dart） | 真正语言中立、可复用的 C++ 底座 |
|---|---|
| `ui.SceneBuilder` (compositing.dart) | `flutter::LayerTree` / `flow::Layer` 体系 |
| `ui.Canvas` (painting.dart) | `flutter::DisplayListBuilder` / `DlCanvas` |
| `ui.Picture` (painting.dart) | `flutter::DisplayList` |
| `ui.Codec` / `Image` (painting.dart) | `flutter::ImageDecoder` / `DlImage`（你在 GIF 链路里见过）|
| `ui.PlatformDispatcher` (platform_dispatcher.dart) | engine 的 `PlatformView` / `VsyncWaiter` / `PointerDataPacket` C++ 端 |

`lib/ui/painting/*.cc`（canvas.cc / picture.cc / codec.cc …）证明了这点：它们全是"Dart 类 ↔ C++ 对象"的胶水，底下封装的是 `flutter::DisplayListBuilder`、`flutter::DisplayList`、`DlImage` 这些**纯 C++、不碰 Dart 的对象**。**去 runtime 后，丢掉胶水，直接拿底下的 C++ 对象用。**

### 0.A.4 这条路的本质：把 Flutter engine 当成"C++ 渲染后端"，自建一套 C++ UI

把上面三点合起来，"去 Dart runtime" 的真实形态是：

```
  自写 C++ 业务逻辑
        │
  自写 C++ UI 框架（Widget/Element/RenderObject 三棵树的 C++ 重建）
        │
  flow::LayerTree / DisplayListBuilder      ← 复用 engine 这一层（语言中立）
        │
  Skia / Impeller + 平台 surface / vsync    ← 复用 engine 这一层（语言中立）
```

也就是：**fork engine → 砍掉整个 Dart 层（dart:ui 的 .dart + VM 集成）→ 在 flow 层之上用 C++ 重建 UI 框架 + 业务**。结果是一个"用了 Flutter 渲染引擎的 C++ UI 框架"，**它不再是 Flutter**（没有 Dart、没有 pub 生态、没有 hot reload、没有现成的 material/cupertino）。

### 0.A.5 去 runtime 必须先回答的三个问题

在动手前，这三个问题决定可行性，**比任何代码都重要**：

1. **业务逻辑用什么语言写？** UI 框架 C++ 化只解决 ②③④，你的 app 逻辑（①）是 C++？Rust？还是嵌一个轻量脚本（Lua/QuickJS）？这是工作量大头，原文档完全没覆盖。
2. **material/cupertino 怎么办？** 这 20 万行是 Flutter "开箱即用"的核心价值。去 Dart 后要么 C++ 全量重写（天文数字），要么自己做一套精简控件集（等于放弃 Flutter 的 UI 资产）。
3. **图片/文本/平台通道这些"看似框架、实则跨 Dart-C++"的子系统**：文本排版（`text/`）、图片解码（你熟的 codec，已大量 C++ 但入口在 Dart）、`MethodChannel`（平台插件全靠它，纯 Dart 协议）——每一个都要重接。插件生态基本归零。

### 0.A.6 体量重估（相比原文档的 18–30 人月）

原文档的 18–30 人月**只算了三棵树、且假设保留 Dart 外壳跑业务**。去 runtime 推翻了这个假设，要叠加：

- ① 业务层脱 Dart：**取决于 app 规模，不可估**（这是你自己的代码）。
- ② material/cupertino 重建：若全量 C++，**+数十人月**；若只做精简控件集，看裁剪程度。
- ③ dart:ui 全套封装在 C++ 侧重建（文本、图片、平台通道、语义/无障碍）：**+10~20 人月**。
- 插件生态：**归零**，按需逐个重写。

> 现实结论：**纯去 runtime 不是"迁移框架"，是"基于 Flutter 渲染引擎从零造一个 C++ UI 框架"。** 这是 Qt / 自研引擎级别的命题。除非有硬约束（见下），否则要极度谨慎。

### 0.A.7 什么情况下"去 Dart runtime"才真正成立

只有满足以下之一，这笔投入才划算：

- **极端体积约束**：产物要砍到几百 KB 级（Dart VM + snapshot 是 MB 级），用于深嵌入式 / IoT / 固件。
- **宿主已是大型 C++ 系统**：游戏引擎、CAD、车机底层，要求 UI 树被 C++ 直接持有、与现有内存/线程模型统一，不容忍第二个 runtime。
- **合规/供应链**：禁止引入 Dart VM（JIT 安全审计、特定平台不允许带 VM）。

如果你的动机不在这三类里（比如其实只是"想要更快/更省内存"），**请回到 §8 方案 1（只下沉热点）**——那个用 1/10 的代价拿到 80% 的收益，且保住整个 Flutter 生态。

---

## 0.B 业务层定案：C++ 主干 + JS 可选动态层（quickjs / v8）

已确定：**业务逻辑用 C++ 写，后续提供 JS binding（quickjs / v8）让 JS 也能驱动同一套 C++ UI 内核。** 这个决定很关键——它把整个架构从"单纯去 Dart"升级成"语言中立内核 + 可插拔前端"，跟原文档 §2 的 `WidgetDesc` 设计天然契合，但对内核 API 设计施加了**现在就必须遵守**的约束。

### 0.B.1 终态分层

```
前端层（可插拔，同一套内核）
  ├─ C++ 业务         ← 主干，直接调内核 C++ API（零开销）
  └─ JS 业务（可选）   ← 通过 binding 调内核，运行期动态加载
        │
   ─────┼───── 语言中立 ABI 边界（关键：现在就要钉死）
        ▼
C++ UI 内核（三棵树：Element / RenderObject / BuildPipeline）
        │
flow::LayerTree / DisplayListBuilder / Skia·Impeller（复用 engine）
```

**核心原则**：C++ 业务和 JS 业务走**同一个内核 API**，区别只在"JS 那一侧多一层 binding 包装"。内核自己**永远不感知**上层是 C++ 还是 JS——这正是 §3.1 `WidgetDesc.build` 设计成"语言中立回调"的意义：C++ 业务传 C++ lambda，JS 业务传一个"能回调进 JS 引擎的 trampoline"，内核只认 `std::function`。

### 0.B.2 binding 架构：走 JSI 模式，别走"消息桥"

这是 JS 集成最重要的一个选型，**直接决定性能上限**：

| 模式 | 做法 | 评价 |
|---|---|---|
| ❌ 消息桥（老 React Native bridge） | JS↔C++ 之间 JSON 序列化传消息 | 每帧大量序列化，慢，已被 RN 自己淘汰 |
| ✅ **JSI / HostObject（RN 新架构、Hermes）** | C++ 对象作为 `HostObject` 直接暴露给 JS，JS 持有 C++ 对象引用，调方法零序列化 | **唯一正确选择** |

具体做法：把内核的 `Element` / `State` / `BuildContext` / `WidgetDesc builder` 包成 HostObject，JS 侧 `setState`、`build` 返回的子节点描述，全部是**直接的 C++ 调用**，不经过任何序列化。quickjs 和 v8 都支持这种"C++ 对象 ↔ 脚本对象"的直通绑定（quickjs 用 `JS_NewObjectClass` + opaque 指针，v8 用 `External` / `ObjectTemplate` + internal field）。

### 0.B.3 quickjs vs v8：先做一次"和初衷的一致性检查"

既然去 Dart runtime 的动机里大概率含**体积/嵌入约束**（见 §0.A.7），选引擎时要避免自相矛盾：

| | quickjs | v8 / Hermes |
|---|---|---|
| 体积 | **~700KB–1MB**，和"去 runtime 省体积"一致 | v8 基础就 **10MB+**，等于换了个比 Dart VM 还大的 runtime ⚠️ |
| 性能 | 解释器，够用，但重计算弱 | JIT（v8）/ AOT 字节码（Hermes），强 |
| 启动 | 极快、内存小 | v8 启动慢、内存大；Hermes 折中 |
| 适用 | 嵌入式 / IoT / 体积敏感 / 轻业务脚本 | 重 JS 生态 / 复杂业务 / 桌面 |

> **判断**：如果去 runtime 的核心动机是体积/嵌入，**上 v8 就把省下来的体积又吃回去了，逻辑不自洽**——这种场景选 quickjs（或 Hermes 作为折中）。v8 只在"宿主本来就带 v8"（如 Electron/CEF 环境）或"重 JS 业务必须 JIT"时才划算。
> **建议**：binding 层抽象成引擎无关接口（仿 JSI 的 `Runtime` / `Value` 抽象），底层 quickjs / v8 可替换，避免锁死。

### 0.B.4 跨 GC 生命周期——这是 JS binding 最大的坑

C++ UI 树是 `unique_ptr` 严格所有权（§3.2），而 JS 有**自己的 GC**。当 JS 持有某个 UI 节点 / State 引用时，两套生命周期管理会打架：

- **所有权方向要钉死**：UI 树的所有权**永远在 C++ 内核**，JS 只持有"句柄 / 弱引用"，绝不让 JS GC 决定 C++ 节点的死活。
- **JS→C++ 引用**：JS 持有的 HostObject 内部是 opaque 指针 + 一个 C++ 侧的引用计数 / 代际校验 id（防 use-after-free：C++ 节点已 unmount，JS 还拿着旧句柄调用时，靠 id 失配安全拒绝）。
- **C++→JS 引用**（内核要回调 JS 的 `build` / 事件处理器）：用引擎的持久句柄（quickjs `JS_DupValue` 持有、v8 `Persistent`），并在对应 C++ `Element` 析构时显式释放，否则 JS 闭包永不回收 → 泄漏。
- **Finalizer 兜底**：JS 对象被 GC 时通过 finalizer（quickjs class finalizer / v8 `SetWeak`）通知 C++ 解除引用，但**只作兜底**，不作为主回收路径（GC 时机不可控，不能让 UI 资源等 JS GC）。

> 一句话：**生命周期主导权在 C++，JS 侧一律是"带校验的弱句柄 + 持久句柄显式释放"**，把 GC 不确定性挡在内核之外。

### 0.B.5 即使阶段 0/1 只写 C++，这些 API 约束现在就要遵守

为了将来能无痛挂 JS binding，内核 API 从第一行代码起就要满足：

1. **句柄化、不漏裸指针**：对外（将来给 binding）的对象用稳定句柄（id 或 opaque ptr + 代际校验），不暴露可被脚本悬垂的裸 `Element*`。
2. **行为入口一律 `std::function`，不要编译期模板绑定**：`WidgetDesc.build` / `State.Build` / 事件回调都用可类型擦除的可调用对象——C++ 传 lambda、JS 传 trampoline，内核无差别调用。
3. **属性用类型擦除的 PropertyBag**（§3.1），而非强类型字段——JS 传过来的是动态值，binding 层做一次 marshal 即可，内核不必为每种 widget 写专门的 JS 绑定代码。
4. **错误/异常跨边界要转换**：C++ 异常不能穿过 JS 引擎，binding 层要 catch 并转成 JS 异常（反之亦然）。
5. **线程模型明确**：JS 引擎（尤其 quickjs）通常单线程，UI build 必须固定在 UI 线程跑；layout/paint 可在 worker（参考你熟的 skwasm worker 模型），但 JS 回调只能在 JS 所在线程触发。

> 这条最实在：**阶段 0 PoC 哪怕一行 JS 都没有，只要 API 按上面 5 条设计，将来加 JS binding 就是"加一层包装"，不用重构内核。反之若现在图省事用了强类型模板 + 裸指针，后面挂 JS 要推倒重来。**

---

## 1. 现状与硬边界分析

### 1.1 三棵树职责（当前 Dart 实现）

```
Widget (immutable config)          packages/flutter/lib/src/widgets/framework.dart
  ├─ 不可变、轻量、可重建            abstract class Widget extends DiagnosticableTree   (L312)
  └─ canUpdate(old,new) 决定复用

Element (mutable, 持久)            同文件
  ├─ 真正持久的树，承载生命周期      abstract class Element implements BuildContext     (L3568)
  ├─ InheritedWidget 依赖追踪        class BuildOwner                                   (L2912)
  ├─ dirty 标记 + 重建调度
  └─ RenderObjectElement 负责挂载 RO  abstract class RenderObjectElement                (L6622)

RenderObject (layout + paint)     packages/flutter/lib/src/rendering/object.dart 等 48 文件
  ├─ performLayout / paint / hitTest
  ├─ RenderBox (笛卡尔) / RenderSliver (滚动)
  └─ 产出 Layer 树
```

### 1.2 与 engine 的唯一硬接缝

`RendererBinding.drawFrame`（`rendering/binding.dart` L690 附近）是三棵树通往 engine 的出口：

```
drawFrame():
  rootPipelineOwner.flushLayout()           # RenderObject.performLayout 全树
  rootPipelineOwner.flushCompositingBits()  # 合成位
  rootPipelineOwner.flushPaint()            # RenderObject.paint → 录制 Layer / Picture
  ... compositeFrame()                      # RenderView 用 ui.SceneBuilder 组装 Scene
                                            # → 交给 C++ compositor（这条线以下已是 C++）
```

接缝以下（已经是 C++，**不在本次改造范围**）：
- `dart:ui` 的 `SceneBuilder` / `Scene` / `Layer` / `Picture` / `Canvas`（`engine/.../lib/ui/`）
- Skia / Impeller 光栅化、合成、上屏

**结论**：要迁移的是「接缝以上的整套 Dart 框架」，迁移完成后需要把新的 C++ 框架重新接到 `SceneBuilder`/`Canvas` 这层 C++ API 上（这反而简单，因为是 C++→C++）。

### 1.3 框架对 Dart runtime 的隐性依赖（迁移难点的根源）

这些是把代码从 Dart 搬到 C++ 时真正棘手的地方：

1. **闭包与 `build(BuildContext)`**：用户的 `build` 方法是 Dart 闭包，捕获词法作用域。C++ 内核如何调用、如何持有？
2. **`==` / `hashCode` / `Key`**：diff 依赖 Dart 的相等语义（尤其 `const` widget 的规范化）。
3. **`InheritedWidget` 依赖追踪**：`dependOnInheritedWidgetOfExactType<T>()` 用 Dart 泛型 + 运行时类型。
4. **GC 管理生命周期**：Element/RenderObject 的销毁依赖 Dart GC + `dispose` 约定，C++ 要换成显式 RAII / 引用计数。
5. **hot reload**：依赖 Dart VM 的 `reassemble`。C++ 化基本等于**放弃 hot reload**（除非自建热替换，代价极高）。
6. **`async`/`Future`/`Stream`/`Timer`**：动画 ticker、图片解码、手势等大量异步逻辑绑定 Dart event loop。
7. **反射式诊断**：`DiagnosticableTree`、widget inspector、`toStringDeep` 依赖 Dart 运行时信息。

---

## 2. 总体架构：C++ 内核 + 语言中立配置层

```
┌─────────────────────────────────────────────────────────────┐
│  前端声明层（可插拔）                                          │
│   ├─ Dart DSL（保留现有 Widget 写法，编译期/运行期转 C++ 节点）│
│   ├─ C++ DSL（fluent builder / 宏）                            │
│   └─ 其他语言 binding（可选）                                  │
└───────────────────────────┬─────────────────────────────────┘
                            │  WidgetDesc（语言中立的不可变配置描述，POD/variant）
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  C++ 框架内核（本次迁移核心）                                  │
│   ├─ ElementTree：生命周期、diff/reconcile、dirty 调度         │
│   ├─ InheritedScope：依赖追踪（类型 id 索引）                  │
│   ├─ BuildPipeline：rebuild → layout → paint 调度             │
│   └─ RenderTree：RenderObject（layout/paint/hitTest）         │
└───────────────────────────┬─────────────────────────────────┘
                            │  接到现有 C++ 出口
                            ▼
┌─────────────────────────────────────────────────────────────┐
│  已有 C++ engine（不改）                                       │
│   SceneBuilder / Layer / Canvas / Picture → Skia/Impeller     │
└─────────────────────────────────────────────────────────────┘
```

核心设计原则：**把「配置（Widget）」与「行为（build 逻辑）」解耦成语言中立的数据 + 回调**，让 C++ 内核只认 `WidgetDesc` 和函数指针/虚函数，不关心上层是 Dart 还是 C++。

---

## 3. 核心子系统迁移设计

### 3.1 Widget → `WidgetDesc`（语言中立配置）

把 Dart 的 `Widget` 抽象成 C++ 侧的不可变描述节点：

```cpp
// 语言中立的 widget 描述。由前端层产生，内核只读。
struct WidgetDesc {
  WidgetTypeId type;            // 稳定类型 id（取代 runtimeType）
  KeyId key;                    // 取代 Key，0 表示无 key
  // 配置数据：POD 字段 or 类型擦除的属性包
  PropertyBag props;
  // 子节点
  std::vector<WidgetDescRef> children;

  // 行为入口（三选一，取决于 widget 种类）
  BuildFn build;                // StatelessWidget / State.build：返回子 WidgetDesc
  CreateStateFn create_state;   // StatefulWidget
  CreateRenderObjectFn create_ro; // RenderObjectWidget
  UpdateRenderObjectFn update_ro;
};
```

- `canUpdate(old, new)` → `old.type == new.type && old.key == new.key`（与现有语义一致）。
- `build` / `create_state` 是**回调**：Dart 外壳模式下指向一个能回调进 Dart VM 的 trampoline；纯 C++ 模式下就是普通 C++ 函数/lambda。
- `const` widget 规范化 → 前端层负责（Dart 编译期 const，或 C++ 侧做 desc 去重缓存）。

### 3.2 Element 树 → C++ `Element`（最核心、最难）

Element 是真正持久、有生命周期的树。C++ 化要点：

```cpp
class Element {
 public:
  virtual void Mount(Element* parent, Slot slot);
  virtual void Update(WidgetDescRef new_desc);   // 复用同一 Element，换配置
  virtual void Unmount();
  virtual void PerformRebuild();                 // dirty 时重建

  // 取代 GC：显式所有权
  void UpdateChild(std::unique_ptr<Element>& child,
                   WidgetDescRef new_desc, Slot slot); // 等价 Dart updateChild

  ElementLifecycle lifecycle() const;            // initial/active/inactive/defunct
 protected:
  WidgetDescRef desc_;
  Element* parent_ = nullptr;
  BuildOwner* owner_ = nullptr;
};
```

关键机制逐条对应：

| Dart 机制 | C++ 实现 |
|---|---|
| `updateChild(child, newWidget, slot)` 四象限（增/删/改/换） | 同算法，用 `std::unique_ptr<Element>` 表达所有权；返回新 child |
| `Key` 匹配（`updateChildren` 列表 diff） | 复刻现有 list-diff（首尾扫描 + key map），key 用整数 id |
| `markNeedsBuild` + `BuildOwner.scheduleBuildFor` | `dirty_elements_` 列表 + 排序（按深度）；`BuildScope` |
| `inactiveElements`（GlobalKey 跨树移动） | 显式 inactive 池 + 帧末 `unmount` 回收 |
| `mount/unmount/activate/deactivate` 生命周期 | 同名虚函数；用 RAII 保证 `unmount` 必被调用 |
| `dependencies`（InheritedWidget） | 见 §3.4 |

**生命周期最大风险**：Dart 靠 GC 兜底"忘记 dispose"的情况，C++ 没有。必须用 `unique_ptr` 严格表达父子所有权，`GlobalKey` 重定位时用"先 deactivate 进池、再 activate 出池、帧末统一 unmount"的两阶段回收，杜绝悬垂指针。

### 3.3 RenderObject 树 → C++ `RenderObject`

这一层 C++ 化**最自然**——它本来就是数据 + 算法，几乎没有 Dart 特性依赖：

```cpp
class RenderObject {
 public:
  void LayoutAsBoundary(const Constraints&);    // layout 入口
  virtual void PerformLayout() = 0;
  virtual void Paint(PaintContext&, Offset) = 0;
  virtual bool HitTest(HitTestResult&, Offset position);

  void MarkNeedsLayout();
  void MarkNeedsPaint();

  ParentData* parent_data = nullptr;            // 取代 Dart parentData
 protected:
  Constraints constraints_;
  bool needs_layout_ = true;
  RenderObject* relayout_boundary_ = nullptr;
};

class RenderBox : public RenderObject {
  Size size_;
  // intrinsic 尺寸、baseline 等
};
```

要点：
- `performLayout` / `paint` / `hitTest` 是纯算法，直接翻译。
- `markNeedsLayout` 的 `relayoutBoundary` 优化（脏标记冒泡到边界）直接复刻。
- `paint` 阶段调用 dart:ui `Canvas` 的部分 → 改调 C++ 的 `Canvas`/`DisplayListBuilder`（你在 skwasm 已经接触过的 `canvas_drawImage` 那层 C++ API）。**这是迁移后重新接缝的地方，C++→C++，反而比现在 Dart→C++ 的 FFI 更直接。**
- `Layer` 树同样 C++ 化，最终 `compositeFrame` 直接调 C++ `SceneBuilder`。

### 3.4 InheritedWidget 依赖追踪 → `InheritedScope`

Dart 用泛型 + 运行时类型 + `_inheritedElements` map。C++ 替代：

```cpp
// 每个 Element 维护：从根到此处可见的 InheritedElement 索引
using InheritedMap = absl::flat_hash_map<WidgetTypeId, InheritedElement*>;

class InheritedElement : public Element {
  std::unordered_set<Element*> dependents_;   // 依赖我的后代
  void NotifyDependents();                     // 数据变更时标脏所有 dependent
};

// dependOnInheritedWidgetOfExactType<T>()  →
InheritedElement* DependOnInherited(Element* ctx, WidgetTypeId t) {
  auto* ie = ctx->inherited_map_[t];           // O(1) 查找
  if (ie) ie->dependents_.insert(ctx);
  return ie;
}
```

- 泛型 `<T>` → 编译期为每个 Inherited 类型分配稳定 `WidgetTypeId`。
- `updateShouldNotify` → 回调/虚函数。

### 3.5 State（StatefulWidget）

```cpp
class State {
 public:
  virtual void InitState() {}
  virtual void DidChangeDependencies() {}
  virtual void DidUpdateWidget(WidgetDescRef old) {}
  virtual void Dispose() {}
  virtual WidgetDescRef Build(BuildContext) = 0;

  void SetState(std::function<void()> fn) {     // setState
    fn();
    element_->MarkNeedsBuild();
  }
 protected:
  StatefulElement* element_;
};
```

Dart 外壳模式下，`State` 是一个持有 Dart 对象句柄的 C++ 壳，`Build` 回调进 Dart；纯 C++ 模式下直接子类化。

### 3.6 调度、动画、手势（依赖 event loop，需专门处理）

- **SchedulerBinding / vsync**：当前 `WidgetsBinding` 由 Dart 驱动 `handleBeginFrame/handleDrawFrame`。C++ 化后由 C++ 直接接 engine 的 vsync 回调（engine 本就用 C++ 发 vsync）。
- **Ticker / AnimationController**：依赖 `Ticker` 订阅 vsync + Dart `Duration`。C++ 重写为基于 vsync 时间戳的 ticker；缓动曲线（`Curve`）是纯数学，直接翻译。
- **手势 `GestureArena`**：纯状态机 + 指针事件路由，C++ 化直接。指针事件本来就从 C++ engine 进来（`pointer.dart` 包装的是 C++ `PointerData`）。
- **异步**：图片解码（你熟悉的 codec 链路）等已大量在 C++；Dart `Future` 边界改为 C++ 回调/协程。

---

## 4. Dart 互操作策略（决定可行性的关键）

如果保留 Dart 写业务（推荐，避免重写海量 material/cupertino），需要一套高效的 Dart ↔ C++ 内核桥：

### 方案 A：Dart 作为"配置生产者"，C++ 持有树（推荐）
- 用户写的 `build()` 仍是 Dart，但返回的不是 Dart Widget 对象，而是通过 binding 直接构造 C++ `WidgetDesc`（类似 React 的 `createElement` 调用）。
- C++ 内核驱动 diff/layout/paint，需要 rebuild 某个 StatefulElement 时，通过 `dart_api` 回调对应的 Dart `build` 闭包。
- **桥的代价**：每帧脏 element 的 build 要跨 FFI 回 Dart。需用 `Dart_PersistentHandle` 持有 State/闭包，批量回调减少边界开销。

### 方案 B：编译期把 Dart Widget 转译为 C++（激进）
- 写一个 transformer（Dart kernel → C++ codegen），把 `StatelessWidget.build` 编译成 C++。
- 仅适用于"无动态闭包捕获"的纯声明式 widget，遇到复杂逻辑退化。**不现实做全量**，可作热点优化补充。

### 方案 C：完全放弃 Dart，纯 C++ 前端
- 重写 material/cupertino/widgets 全套（>400 个 Dart 文件，~20 万行）为 C++。
- 工作量天文数字，且失去 Flutter 生态。**仅在"去 Dart runtime"是硬需求时考虑**。

**推荐：方案 A 为主线，方案 B 作为热点 widget（如 Text/Container/Flex）的可选加速。**

---

## 5. 分阶段实施路线图

> 原则：**自底向上、保持可运行、每阶段都能跑现有 app**。先迁最不依赖 Dart 特性的 RenderObject 层，最后才碰 Element/Widget。

### 阶段 0：可行性验证（PoC，1–2 月）
- 选最小闭环：`RenderView → RenderPadding → RenderColoredBox`（一个带背景色和 padding 的盒子）。
- 在 C++ 实现这 3 个 RenderObject + 一个极简 C++ Element 树 + 直接构造 desc（先不接 Dart）。
- 接到现有 C++ `SceneBuilder`，能在真机/web 上画出一个有色方块。
- **验收**：证明 C++ 框架能独立产出一帧，跑通 layout→paint→composite→上屏。

### 阶段 1：RenderObject 层全量 C++ 化（3–6 月）
- 迁移 `rendering/` 48 个文件的核心：`RenderBox`、`RenderSliver`、`RenderFlex`、`RenderStack`、`RenderParagraph`、`RenderImage`、`Layer` 体系。
- 现有 Dart `RenderObject` 改为 C++ 对象的**薄 FFI 包装**（Dart 侧 `RenderFlex` 只是 C++ `RenderFlex*` 的句柄），保证现有 Element 层无感。
- **验收**：现有 Dart app 不改一行，layout/paint 跑在 C++ 上，性能对比基线。

### 阶段 2：Element / BuildOwner 内核 C++ 化（6–12 月）
- C++ 实现 `Element`、`BuildOwner`、diff（`updateChild`/`updateChildren`）、生命周期、`InheritedScope`、脏标记调度。
- Dart 侧 Widget 通过 binding 构造 `WidgetDesc`；`build` 回调跨 FFI（方案 A）。
- 最难的部分：`GlobalKey` 重定位、`InheritedWidget` 依赖、`State` 生命周期跨语言。
- **验收**：复杂 app（带 Navigator/动画/列表）行为与原版一致，通过 framework 测试套件。

### 阶段 3：调度 / 动画 / 手势 C++ 化（与阶段 2 并行部分）
- vsync 调度、Ticker、GestureArena 下沉 C++。

### 阶段 4：优化与收口（持续）
- 热点 widget 走方案 B 编译期转译。
- 内存/生命周期审计（ASAN/LSAN），补齐诊断/inspector（重建 C++ 侧 diagnostics）。

> **总量级**：以小型专职团队（5–8 人）计，达到"复杂 app 可用"约需 **18–30 个月**。这不是一个季度能完成的事。

---

## 6. 测试与质量保障

- **行为等价测试**：现有 `flutter/test` 的 widget 测试套件是最强的回归网。每阶段必须保持绿色（Dart 外壳模式下可直接复用）。
- **golden 测试**：像素级对比，确保 layout/paint 迁移无视觉回归。
- **内存安全**：C++ 引入悬垂/泄漏风险，全程 ASAN/LSAN + 严格 `unique_ptr` 所有权模型 + GlobalKey 两阶段回收审计。
- **性能基准**：`dev/benchmarks` 现有跑分，每阶段对比，防止"为了 C++ 反而更慢"（FFI 边界、缓存不友好等）。

---

## 7. 主要风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| **失去 hot reload** | C++ 无 Dart VM 热替换，开发体验断崖 | 保留 Dart 外壳（业务仍 Dart）；或投入自建热替换（代价极高） |
| **FFI 边界开销** | 每帧脏 element build 跨语言回调 | 批量回调、`PersistentHandle` 缓存、热点 widget 转译 |
| **生命周期/内存安全** | 失去 GC 兜底，悬垂指针风险 | 严格所有权模型、ASAN、两阶段回收 |
| **生态断裂** | pub 包大量依赖 Dart framework API | 保持 Dart 外壳 API 兼容；否则生态归零 |
| **维护分叉** | 偏离上游 Flutter，无法跟随更新 | 评估是否值得长期 fork；考虑只 fork 内核、跟随上游 widget 层 |
| **投入产出比** | 18–30 人月，收益依赖动机 | 回到 §0：若动机是性能，先做局部优化而非整套迁移 |

---

## 8. 替代/对比方案

1. **只下沉热点（最务实）**：保持框架在 Dart，只把 `RenderFlex`/`RenderParagraph` 等 layout 热点用 C++ 重写并 FFI 暴露。投入小、风险低、收益可量化。**若动机是性能，首选这个。**
2. **整套 C++ + 自有 DSL（最激进）**：放弃 Dart，对标 Qt/自研 UI 引擎。仅在嵌入式/去 runtime 硬需求下成立。
3. **Rust 而非 C++**：内核用 Rust（所有权模型天然适配 Element 生命周期，内存更安全）。若是 greenfield，值得认真评估。
4. **不迁移，改用 Impeller/优化 rebuild**：很多"性能问题"实际是业务侧 rebuild 过度或光栅化瓶颈，调优成本远低于重写框架。

---

## 9. 立即可执行的下一步（建议）

1. **明确动机**（§0 表格选一行）——这是一切的前提，先回答"为什么"。
2. **做阶段 0 PoC**：C++ 实现 `RenderColoredBox + RenderPadding + RenderView` 接 `SceneBuilder`，画出一个方块。两周内可验证最大的未知：C++ 框架能否独立产出一帧并上屏。
3. **基线性能采集**：在 PoC 同时，用 `dev/benchmarks` 采集现有 Dart 框架的 build/layout/paint 耗时，作为后续每阶段的对照。

---

### 附：关键代码锚点（便于后续实现对照）

- Widget/Element/BuildOwner：`packages/flutter/lib/src/widgets/framework.dart`（L312 / L2912 / L3568 / L6622）
- RenderObject 核心：`packages/flutter/lib/src/rendering/object.dart`
- 帧管线出口：`packages/flutter/lib/src/rendering/binding.dart` `drawFrame`（L690 附近）
- dart:ui 接缝（迁移后重接点）：`engine/src/flutter/lib/ui/compositing.dart`（SceneBuilder）、`painting.dart`（Canvas/Picture）
- C++ 侧已有渲染出口参考：`engine/src/flutter/skwasm/canvas.cc`、`surface.cc`（你已熟悉的 `canvas_drawImage` / `surface_renderPictures` 那层）
