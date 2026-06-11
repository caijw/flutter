# 全 C++ UI 内核 + JS 动态化（quickjs / v8 双引擎）—— 完整技术方案

> 一句话定位：**fork Flutter engine，砍掉 Dart 层（dart:ui + Dart VM + framework），在 flow / DisplayList / Skia·Impeller 之上用 C++ 重建一套 Widget / Element / RenderObject 三棵树，对外暴露语言中立 ABI，并在其上挂载 quickjs / v8 双引擎可切换的 JS 前端，作为业务动态化方案。**

> 本文是独立完整方案，不依赖也不延续 `cpp-framework-migration-plan.md` 的渐进路线（那条线保留 Dart 外壳；本方案彻底不要 Dart）。

---

## 0. 目标与非目标

### 0.1 目标
1. **彻底去掉 Dart**：Dart VM、dart:ui、framework（widgets/material/cupertino）、pub 生态——**全部不要**。产物里没有任何 Dart 字节码或 snapshot。
2. **C++ 重建三棵树**：`Widget`（不可变描述）/ `Element`（持久生命周期）/ `RenderObject`（layout/paint/hitTest），算法语义对齐 Flutter 当前实现，但不绑定 Dart。
3. **复用 engine 渲染底座**：`flow::LayerTree`、`DisplayListBuilder`、Skia / Impeller、平台 surface / vsync / pointer 这一层语言中立，**保留并直接 C++ 调用**。
4. **JS 动态化**：业务可用 JS 编写，运行期下发/热更。**同时支持 quickjs 与 v8**，二选一编译，API 对业务无差别。
5. **C++ 业务也能跑**：JS 是动态层，核心业务允许直接用 C++ 写，与 JS 共用同一内核（同一套 API、同一棵树）。

### 0.2 非目标（明确不做）
- **不保留 hot reload**（Dart VM 才有的能力）。JS 侧用"重载脚本 + 重建子树"代替，不是同一个东西。
- **不复刻 material / cupertino 全量控件**。只提供基础控件集（Box/Flex/Stack/Text/Image/Scroll/Input/Touch 等约 20–40 个），上层主题/复合控件由 JS 业务方自建或后续逐步补。
- **不兼容现有 pub 包**。生态归零，按需重写。
- **不做"Dart 调 JS"或"JS 调 Dart"**——Dart 已经不存在。

### 0.3 适用前提（不满足请回头）
本方案投入巨大（团队规模 8–15 人 × 24–36 月级别），只在以下硬约束之一成立时才划算：
- **强动态化诉求**：业务必须能不发版热更新（电商/内容/活动类 App 典型诉求），且对 Flutter 现有方案（受限于 Dart AOT）不满意。
- **统一容器多端复用**：一套 C++ 内核同时跑 iOS/Android/桌面/嵌入式/小程序容器，JS 写一次多端运行。
- **嵌入大型 C++ 宿主**：游戏引擎 / 车机 / IoT，要求 UI 与宿主同语言、同内存模型，不容忍第二个 runtime。
- **体积合规**：禁止引入 Dart VM，且 quickjs 这个量级（~700KB）可接受。

> 如果只是想"性能好一点"——这套方案不解决问题，请用渐进迁移（见旧文档 §8 方案 1）。

---

## 1. 总体架构

```
┌────────────────────────────────────────────────────────────────┐
│  业务层（前端，可插拔，同一内核）                                  │
│   ├─ JS 业务（主动态层）  ──┐                                    │
│   │   ├─ quickjs runtime    │  通过 JsiLikeRuntime 抽象接入       │
│   │   └─ v8 runtime         │  （二选一，编译期决定）              │
│   └─ C++ 业务（静态主干，零开销直调内核）                          │
└──────────────────────────────┬─────────────────────────────────┘
                               │ 语言中立 ABI（C 头文件 + 句柄）
                               ▼
┌────────────────────────────────────────────────────────────────┐
│  C++ UI 内核（本方案核心，新写）                                   │
│   ├─ WidgetDesc       不可变配置（POD + PropertyBag + children） │
│   ├─ ElementTree      持久树、diff/reconcile、生命周期、脏调度   │
│   ├─ InheritedScope   依赖追踪（typeId → InheritedElement）      │
│   ├─ State / Hooks    StatefulWidget 等价物                      │
│   ├─ BuildPipeline    rebuild → layout → paint 调度              │
│   ├─ RenderTree       RenderBox / RenderSliver / RenderFlex …   │
│   ├─ GestureSystem    指针事件路由 + GestureArena                │
│   ├─ AnimationSystem  vsync 驱动的 Ticker / Curve / Tween        │
│   ├─ TextEngine       排版（接 SkParagraph / minikin）           │
│   ├─ AssetSystem      图片解码 / 字体 / 资源包                   │
│   └─ PlatformChannel  原生能力调用（替代 MethodChannel）         │
└──────────────────────────────┬─────────────────────────────────┘
                               │ C++ → C++ 直调
                               ▼
┌────────────────────────────────────────────────────────────────┐
│  engine 复用层（不改，跟随上游）                                   │
│   flow::LayerTree / DisplayListBuilder / DlCanvas               │
│   Skia + Impeller / Surface / VsyncWaiter / PointerDataPacket   │
└────────────────────────────────────────────────────────────────┘
```

**核心原则**：
1. **内核语言中立**：内核 API 不出现 Dart、不出现 JS、不出现 quickjs/v8 任何符号。所有上层语言通过 binding 适配进来。
2. **JS 是"前端层 + 动态化协议"**，不是内核的一部分。撤掉 JS 引擎，C++ 业务能独立跑。
3. **生命周期主权在 C++**：JS 持有的永远是带校验的句柄，不让 JS GC 决定 C++ 节点死活。

---

## 2. 复用边界：engine 这一层留什么、砍什么

### 2.1 直接复用（不动，跟随上游）

| 子系统 | 路径 | 说明 |
|---|---|---|
| Layer 树 | `engine/src/flutter/flow/layers/layer_tree.h` | 合成层，paint 阶段产物 |
| 绘制录制 | `engine/src/flutter/display_list/dl_builder.h` (`DisplayListBuilder`) | 取代 `ui.Canvas`，paint 直接调它 |
| 绘制回放 | `engine/src/flutter/display_list/display_list.h` | 等价 `ui.Picture` |
| 合成上下文 | `engine/src/flutter/flow/compositor_context.{h,cc}` | 合成入口 |
| Surface | `engine/src/flutter/flow/surface.h` + 各平台实现 | 输出目标 |
| Vsync | `engine/src/flutter/shell/common/vsync_waiter.h` | 帧调度信号 |
| Pointer | `engine/src/flutter/lib/ui/window/pointer_data.h` + `pointer_data_packet.h` | 输入事件，注意现在是在 lib/ui 下，需要把 PointerDataPacket 这个**纯数据结构**抽到不依赖 Dart 的目录（或直接复制定义） |
| 图片解码 | `engine/src/flutter/lib/ui/painting/image_decoder.h` 关联的 C++ 部分 | 现状是 Dart 入口 + C++ 实现，砍掉 Dart 入口直接拿底层 C++ 用 |
| 文本排版 | `third_party/skia` 的 `SkParagraph`、平台原生（Core Text / Minikin） | 直接 C++ 调用 |

### 2.2 砍掉（跟着 Dart 一起拆）

| 子系统 | 路径 | 处置 |
|---|---|---|
| Dart VM 集成 | `engine/src/flutter/runtime/`、`shell/common/engine.{cc,h}` 的 Dart 部分 | **整段删除** |
| dart:ui 实现 | `engine/src/flutter/lib/ui/**.dart` + 对应 `_natives.cc` 胶水 | **整段删除** |
| 标准 embedder API | `engine/src/flutter/shell/platform/embedder/` | **不能用**（其入参强依赖 Dart snapshot），新写一套 C 头 ABI（见 §8） |
| Flutter framework | `packages/flutter/`、`packages/flutter_test/` 等 | **整段删除**（C++ 重写） |
| Flutter Tools | `packages/flutter_tools/` | **整段删除**，新写构建工具（见 §11） |

### 2.3 接缝：内核如何接到 engine

```cpp
// 简化伪代码：每帧的内核出口
void UiKernel::DrawFrame(fml::TimePoint frame_target) {
  build_owner_->BuildScope([&]{ /* 跑脏 element 的 build */ });
  pipeline_owner_->FlushLayout();      // RenderObject.PerformLayout
  pipeline_owner_->FlushCompositingBits();
  pipeline_owner_->FlushPaint();       // RenderObject.Paint → DisplayListBuilder
  auto layer_tree = render_view_->BuildLayerTree();   // 产出 flow::LayerTree
  rasterizer_->Draw(std::move(layer_tree));           // 交给 engine 已有的 rasterizer
}
```

`Rasterizer` / `Shell` 这层 engine 已有，砍掉它对 Dart 的依赖（runtime controller）后即可直接驱动。

---

## 3. C++ UI 内核：核心数据结构与 API

### 3.1 `WidgetDesc`：语言中立的不可变配置

```cpp
namespace ui {

using WidgetTypeId = uint32_t;   // 全局唯一类型 id（含内置类型 + 用户自定义）
using KeyId        = uint64_t;   // 0 = 无 key

// 类型擦除属性包：兼容 C++ 直接构造与 JS marshal
class PropertyBag {
 public:
  void Set(StringRef name, Value v);
  const Value* Get(StringRef name) const;
  // Value: variant<bool, int64, double, string, Color, Offset, Size,
  //                EdgeInsets, Alignment, Handle<Image>, OpaqueRef, ...>
};

class WidgetDesc final : public RefCounted<WidgetDesc> {
 public:
  WidgetTypeId type;
  KeyId        key{0};
  PropertyBag  props;
  std::vector<RefPtr<WidgetDesc>> children;

  // 行为入口（按节点种类只用其一）：
  BuildFn               build;          // Stateless / State.Build
  CreateStateFn         create_state;   // Stateful
  CreateRenderObjectFn  create_ro;      // RenderObjectWidget
  UpdateRenderObjectFn  update_ro;
};

using BuildFn              = std::function<RefPtr<WidgetDesc>(BuildContext*)>;
using CreateStateFn        = std::function<std::unique_ptr<State>()>;
using CreateRenderObjectFn = std::function<std::unique_ptr<RenderObject>(BuildContext*)>;
using UpdateRenderObjectFn = std::function<void(RenderObject*, const PropertyBag&)>;

}  // namespace ui
```

设计要点：
- `std::function` 而非模板：C++ 业务传 lambda、JS 业务传 trampoline，内核统一调用，零侵入。
- `PropertyBag` 字段名用 interned `StringRef`（指向常量池），查找走小哈希，避免每帧字符串分配。
- `WidgetDesc` 引用计数（`RefPtr`）是因为同一份 desc 可能被多 element 引用（key 重定位、列表 diff 暂存），简化所有权。
- `canUpdate(old, new)` ≡ `old.type == new.type && old.key == new.key`，与 Flutter 对齐。

### 3.2 `Element` 树：持久态、diff、生命周期

```cpp
enum class ElementLifecycle { kInitial, kActive, kInactive, kDefunct };

class Element {
 public:
  // Flutter 等价 API：
  virtual void Mount(Element* parent, Slot slot);
  virtual void Update(RefPtr<WidgetDesc> new_desc);
  virtual void Unmount();
  virtual void PerformRebuild();

  // updateChild 的等价物，强所有权
  std::unique_ptr<Element> UpdateChild(std::unique_ptr<Element> child,
                                       RefPtr<WidgetDesc> new_desc, Slot slot);

  // 列表 diff（updateChildren 等价）
  std::vector<std::unique_ptr<Element>> UpdateChildren(
      std::vector<std::unique_ptr<Element>> old_children,
      std::vector<RefPtr<WidgetDesc>> new_descs);

  // BuildContext 等价
  RenderObject* FindAncestorRenderObjectOfType(RenderObjectKind);
  InheritedElement* DependOnInherited(WidgetTypeId t);

  ElementLifecycle lifecycle() const { return lifecycle_; }
  void MarkNeedsBuild();   // 等价 markNeedsBuild

 protected:
  RefPtr<WidgetDesc> desc_;
  Element*           parent_{nullptr};
  Slot               slot_{};
  BuildOwner*        owner_{nullptr};
  ElementLifecycle   lifecycle_{ElementLifecycle::kInitial};

  // 每个 element 缓存"从根到此处可见的 InheritedElement"
  // 父子继承时复用父的 map，仅在 InheritedElement 处复制并覆盖
  const InheritedMap* inherited_map_{nullptr};
};

using InheritedMap = absl::flat_hash_map<WidgetTypeId, InheritedElement*>;
```

**生命周期与所有权**：
- 父用 `std::unique_ptr<Element>` 持有子；移动语义贯穿 diff。
- `GlobalKey` 跨树重定位：用 `BuildOwner::inactive_pool_` 暂存，"先 deactivate 入池 → 再 activate 出池 → 帧末统一 unmount 池中残留"，与 Flutter 现状对齐，杜绝悬垂。
- `Unmount` 由 RAII 兜底（析构必走 unmount 路径，断言 lifecycle == defunct）。
- **不允许裸 `Element*` 跨越 build 边界存活**——任何对外暴露走 `ElementHandle`（id + 代际号）。

**子类**：
- `StatelessElement` —— `PerformRebuild` 调 `desc_->build(this)` 得新 child desc，`UpdateChild`。
- `StatefulElement` —— 持有 `std::unique_ptr<State>`，`PerformRebuild` 调 `state_->Build(this)`。
- `InheritedElement` —— `Update` 时若 `ShouldNotify(old, new)`，遍历 `dependents_` 标脏。
- `RenderObjectElement` —— 创建并持有 `RenderObject`，挂载到父 RO；`Update` 时调 `update_ro`。
- `SingleChildRenderObjectElement` / `MultiChildRenderObjectElement` —— 子节点装载策略。

### 3.3 `BuildOwner`：脏调度与帧管线

```cpp
class BuildOwner {
 public:
  void ScheduleBuildFor(Element* dirty);

  // BuildScope：一帧内的批处理域，按深度 ascending 处理脏 element
  void BuildScope(Element* context, std::function<void()> callback);

  void FinalizeTree();   // 帧末：unmount inactive_pool_ 残留

 private:
  // 按深度排序的最小堆（深度浅的先建，避免重复重建）
  std::vector<Element*> dirty_elements_;
  std::vector<std::unique_ptr<Element>> inactive_pool_;
};
```

### 3.4 `RenderObject` 树

```cpp
class RenderObject {
 public:
  void LayoutAsBoundary(const Constraints& c);   // 外部入口
  virtual void PerformLayout() = 0;
  virtual void Paint(PaintContext&, Offset);
  virtual bool HitTest(HitTestResult&, Offset);

  void MarkNeedsLayout();
  void MarkNeedsPaint();
  void MarkNeedsCompositingBitsUpdate();

  std::unique_ptr<ParentData> parent_data;

 protected:
  Constraints  constraints_;
  bool         needs_layout_{true};
  bool         needs_paint_{true};
  RenderObject* relayout_boundary_{nullptr};
  RenderObject* parent_{nullptr};
};

class RenderBox : public RenderObject { Size size_; /* intrinsics, baseline */ };

// 与 Flutter 对齐的核心 RO 集合（首批要实现的）：
// RenderView / RenderConstrainedBox / RenderPadding / RenderColoredBox /
// RenderDecoratedBox / RenderClipRect / RenderTransform / RenderOpacity /
// RenderFlex / RenderStack / RenderWrap / RenderListBody /
// RenderParagraph / RenderImage /
// RenderViewport / RenderSliverList / RenderSliverGrid /
// RenderRepaintBoundary / RenderOffstage / RenderCustomPaint
```

**Paint 调用**：`Paint(PaintContext&, Offset)` 中拿到的 `PaintContext::canvas()` 是 `flutter::DlCanvas*`（即 `DisplayListBuilder` 的接口），直接调用如 `DrawRect / DrawPath / ClipRect / Save / Transform` 等 —— **C++ → C++ 直调，无任何跨语言开销**。

### 3.5 `InheritedScope`：依赖追踪

每个 `Element` 都引用一个 `const InheritedMap*`：
- 普通元素继承父的 map（指针共享，O(1)）。
- `InheritedElement` 处复制父 map 并写入自身 → 后代沿用新 map。
- `DependOnInherited(typeId)` 是一次哈希查找 + 把 caller 加入 `dependents_` set。
- `InheritedElement::Update` 发现 `ShouldNotify` 时，遍历 `dependents_` 全部 `MarkNeedsBuild`。

### 3.6 `State` / `Hooks`

两套 API 并存，业务自选：

**A. 经典 Stateful 风格**（与 Flutter 对齐）：
```cpp
class State {
 public:
  virtual void InitState() {}
  virtual void DidChangeDependencies() {}
  virtual void DidUpdateWidget(RefPtr<WidgetDesc> old) {}
  virtual void Dispose() {}
  virtual RefPtr<WidgetDesc> Build(BuildContext*) = 0;

  void SetState(std::function<void()> fn) {
    fn();
    element_->MarkNeedsBuild();
  }
 protected:
  StatefulElement* element_;
};
```

**B. Hooks 风格**（推荐 JS 侧主用，更顺手）：
```cpp
// 在 BuildFn 内调用，按调用顺序绑定到当前 element 的 hook 槽
auto [count, setCount] = useState<int>(ctx, 0);
useEffect(ctx, [&]{ /* mount */ }, /*deps=*/{});
auto theme = useInherited<ThemeData>(ctx);
```
内部用"按位置索引"的 hook 槽数组，挂在 `StatefulElement`（或专用 `HookElement`）上。JS 侧的语义与 React Hooks 一致，便于 React 生态人员上手。

### 3.7 调度、动画、手势

- **vsync**：内核注册到 engine `VsyncWaiter`，`OnVsync(frame_time)` → `BuildPipeline::Tick(frame_time)`，依次跑 build / layout / paint / compositing。
- **Ticker / AnimationController**：基于 frame_time 的 delta 推进；`Curve` 是纯数学。
- **GestureArena**：指针事件来源 `PointerDataPacket` → 命中测试 → 路由到 hit-test 路径上的 `GestureRecognizer`s → 仲裁。是纯状态机，C++ 化无难度。

---

## 4. JS 引擎抽象层：JSI-Like Runtime（quickjs / v8 双适配）

这是本方案最关键的工程抽象。目标：**业务侧 JS 代码不感知底层引擎；内核侧 binding 代码也不感知**。引擎切换是编译期开关。

### 4.1 抽象接口（`ui::js::Runtime`）

参照 React Native JSI 简化定制：

```cpp
namespace ui::js {

class Value;          // tagged union: undefined/null/bool/number/string/object/symbol
class Object;         // 对 Value 的窄类型视图
class Function;
class Array;
class HostObject;     // C++ 暴露给 JS 的对象基类
class HostFunction;
class Scope;          // 自动管理引擎栈（quickjs free / v8 HandleScope）
class PersistentRef;  // 跨调用持有 JS 值（quickjs JS_DupValue / v8 Persistent）

class Runtime {
 public:
  // 生命周期
  static std::unique_ptr<Runtime> Create(const RuntimeConfig&);
  virtual ~Runtime();

  // 求值
  virtual Value EvaluateScript(StringRef src, StringRef url) = 0;
  virtual Value EvaluateBytecode(BytesRef bc, StringRef url) = 0;

  // 全局
  virtual Object Global() = 0;

  // 值构造
  virtual Value NewString(StringRef) = 0;
  virtual Value NewNumber(double) = 0;
  virtual Object NewObject() = 0;
  virtual Object NewHostObject(std::shared_ptr<HostObject>) = 0;
  virtual Function NewHostFunction(StringRef name, unsigned argc, HostFn fn) = 0;

  // 调用
  virtual Value Call(const Function&, const Value& thiz,
                     const Value* argv, size_t argc) = 0;

  // 类型/转换
  virtual std::string ToString(const Value&) = 0;
  virtual double      ToNumber(const Value&) = 0;
  // ...

  // 异常
  virtual void ThrowError(StringRef msg) = 0;
  virtual std::optional<JsError> CatchPending() = 0;

  // 持久句柄
  virtual PersistentRef MakePersistent(const Value&) = 0;
  virtual Value         FromPersistent(const PersistentRef&) = 0;

  // GC 提示
  virtual void RequestGc() = 0;
  virtual void OnLowMemory() = 0;
};

class HostObject {
 public:
  virtual ~HostObject() = default;
  virtual Value Get(Runtime&, StringRef name) { return Value::Undefined(); }
  virtual bool  Set(Runtime&, StringRef name, const Value&) { return false; }
  virtual std::vector<std::string> Keys(Runtime&) { return {}; }
};

}  // namespace ui::js
```

### 4.2 quickjs 适配（`ui::js::quickjs::Runtime`）

- `Value` ≈ `JSValue`（tagged 64-bit），持有需要 `JS_DupValue`，离开作用域 `JS_FreeValue`。
- `HostObject` ≈ `JS_NewObjectClass` + 用 `JS_SetOpaque` 挂 C++ 指针，class 上挂 finalizer。
- `HostFunction` ≈ `JS_NewCFunction2`。
- 字节码缓存：`JS_WriteObject(JS_WRITE_OBJ_BYTECODE)` / `JS_ReadObject`，避免每次启动 reparse。

### 4.3 v8 适配（`ui::js::v8::Runtime`）

- `Value` 包装 `v8::Local<v8::Value>`（依赖外层 `Scope` = `v8::HandleScope`）。
- `HostObject` ≈ `ObjectTemplate` + `SetInternalFieldCount(1)` + Internal Field 存 `External(C++ ptr)`，`SetWeak` 注册 finalizer。
- `HostFunction` ≈ `FunctionTemplate::New`。
- 字节码缓存：`v8::ScriptCompiler::CachedData`。
- 隔离：每个 UI 线程一个 `v8::Isolate` + `Locker`。

### 4.4 编译期选择 + 运行期统一

```
build/
  ├─ js_engine = "quickjs"     # 默认（体积友好）
  └─ js_engine = "v8"          # 桌面/重业务可选
```

`Runtime::Create` 的具体实现按 `js_engine` 编译选择，业务代码、binding 代码引入的都是 `ui::js::Runtime` 抽象头，**绝不直接 include `quickjs.h` / `v8.h`**（用 lint 规则强制）。

### 4.5 引擎能力差异处理

| 能力 | quickjs | v8 | 抽象层策略 |
|---|---|---|---|
| Promise / microtask | 自带，需手动跑 `JS_ExecutePendingJob` | 自带，跑 `MicrotaskQueue` | 抽象 `Runtime::DrainMicrotasks()` |
| WeakRef / FinalizationRegistry | ES2021 起支持 | 支持 | 内核**不依赖**它们做主回收，仅诊断 |
| Module（ESM）| 支持 | 支持 | 抽象 `LoadModule(url)` |
| Source map | 自实现 | 自实现 | 由 binding 层错误处理统一处理 |
| Debugger | 自实现 inspector | Chrome DevTools Protocol | 见 §10 |

---

## 5. Widget JS Binding：业务怎么写、内核怎么对接

### 5.1 JS 业务的样子

```js
// 业务侧（JS），与 React 极相似的声明式风格
import { Box, Flex, Text, Image, Touch, useState } from '@ui/core';

function Counter(props) {
  const [n, setN] = useState(0);
  return Flex({
    direction: 'column', mainAxis: 'center', crossAxis: 'center',
    children: [
      Text({ value: `count: ${n}`, fontSize: 20 }),
      Touch({
        onTap: () => setN(n + 1),
        child: Box({ padding: 12, color: '#3478f6',
                     child: Text({ value: 'Tap', color: '#fff' }) }),
      }),
    ],
  });
}

export default Counter;
```

`Box / Flex / Text / ...` 是 binding 层注入到 JS 全局的工厂函数，调用即返回一个**轻量描述对象**（JS 侧只是 `{type, props, children}`，不立刻构造 C++ 对象）。

### 5.2 binding 注入（内核侧）

```cpp
void RegisterCoreWidgetBindings(ui::js::Runtime& rt) {
  auto global = rt.Global();
  global.SetProperty(rt, "Box",   MakeWidgetFactory(rt, kWidgetBox));
  global.SetProperty(rt, "Flex",  MakeWidgetFactory(rt, kWidgetFlex));
  global.SetProperty(rt, "Text",  MakeWidgetFactory(rt, kWidgetText));
  global.SetProperty(rt, "Image", MakeWidgetFactory(rt, kWidgetImage));
  // ... 其余基础控件
  global.SetProperty(rt, "useState",     MakeUseStateBinding(rt));
  global.SetProperty(rt, "useEffect",    MakeUseEffectBinding(rt));
  global.SetProperty(rt, "useInherited", MakeUseInheritedBinding(rt));
}

ui::js::Function MakeWidgetFactory(Runtime& rt, WidgetTypeId type) {
  return rt.NewHostFunction("WidgetFactory", 1, [type](Runtime& r, const Value*, size_t){
    // 实参形如 ({ ...props, children: [...] })
    auto props_obj = /* arg0 */;
    // 不立刻物化 C++ WidgetDesc，先返回一个 JS 描述对象（仅在需要 commit 时再 marshal）
    auto js_desc = r.NewObject();
    js_desc.SetProperty(r, "__type", r.NewNumber(type));
    js_desc.SetProperty(r, "__props", props_obj);
    return Value(js_desc);
  });
}
```

### 5.3 何时把 JS 描述物化为 C++ `WidgetDesc`

两种策略，按 widget 种类选用：

**策略 A：build 闭包方式（用户自定义组件）**
- 用户函数 `Counter` 是 JS 闭包，包到 C++ 侧形成一个 `WidgetDesc`，其 `build` 是一个 trampoline：
  ```cpp
  desc->build = [persistent_fn = rt.MakePersistent(js_counter_fn)](BuildContext* ctx) {
    auto* rt = JsRuntimeOfThisIsolate();
    auto fn  = rt->FromPersistent(persistent_fn).AsFunction();
    auto js_result = rt->Call(fn, {}, /*argv=*/&js_props, 1);
    return MarshalJsToWidgetDesc(*rt, js_result);   // 递归把 JS 描述对象转 C++ desc
  };
  ```
- `MarshalJsToWidgetDesc` 把 `{__type, __props, children}` 递归翻译成 `WidgetDesc`，children 中嵌套的 JS 用户组件本身是个 JS 函数 → 再次包成 build 闭包形式的 `WidgetDesc`。

**策略 B：原生 leaf widget（Box/Text/Image 等）**
- `__type` 是内置 `WidgetTypeId`，binding 层直接构造对应 RenderObjectWidget 的 desc：
  ```cpp
  case kWidgetBox: {
    auto d = MakeRefCounted<WidgetDesc>();
    d->type = kWidgetBox;
    d->key  = ReadKey(props);
    PropsToBag(*rt, props, /*schema=*/kBoxPropsSchema, d->props);
    d->create_ro = [](BuildContext*) { return std::make_unique<RenderColoredBox>(); };
    d->update_ro = &UpdateBoxRO;
    return d;
  }
  ```
- `kBoxPropsSchema` 在 binding 编译期生成（见 §6），把已知字段一次性 marshal，避免运行期反射成本。

### 5.4 `setState` 跨 GC 的实现

```cpp
// useState binding
auto MakeUseStateBinding(ui::js::Runtime& rt) {
  return rt.NewHostFunction("useState", 1, [](Runtime& r, const Value* argv, size_t){
    auto* element = CurrentBuildingElement();   // 通过 thread-local 拿
    auto  slot    = element->NextHookSlot();
    if (!slot->initialized) {
      slot->value = r.MakePersistent(argv[0]);  // 持久句柄
      slot->initialized = true;
    }
    auto setter = r.NewHostFunction("setState", 1, [eh = element->MakeHandle(), slot](
        Runtime& r, const Value* argv, size_t) {
      auto* el = ElementHandle::Resolve(eh);    // 代际校验！失效则 no-op
      if (!el) return Value::Undefined();
      slot->value = r.MakePersistent(argv[0]);
      el->MarkNeedsBuild();
      return Value::Undefined();
    });
    auto pair = r.NewArray(2);
    pair.Set(r, 0, r.FromPersistent(slot->value));
    pair.Set(r, 1, Value(setter));
    return Value(pair);
  });
}
```

**核心安全机制**：
- `ElementHandle = {id: u32, gen: u32}`。每次 `unmount` element 时，全局 element 表的对应 slot 把 `gen` 递增。
- `Resolve(handle)` 检查 `table[id].gen == handle.gen` —— 不匹配返回 nullptr。
- 任何从 JS 进来调用 C++ 的入口都先 `Resolve`，杜绝 use-after-free。

### 5.5 事件回调 C++ → JS

```cpp
class JsEventCallback {
 public:
  JsEventCallback(ui::js::Runtime& rt, const ui::js::Value& fn)
      : rt_(&rt), fn_(rt.MakePersistent(fn)) {}
  ~JsEventCallback() { /* fn_ 持久句柄随对象释放，引擎侧引用计数 -1 */ }

  void Invoke(const EventArgs& args) {
    ui::js::Scope scope(*rt_);
    auto js_args = MarshalEventArgs(*rt_, args);
    auto fn = rt_->FromPersistent(fn_);
    rt_->Call(fn.AsFunction(), {}, js_args.data(), js_args.size());
    rt_->DrainMicrotasks();
    if (auto err = rt_->CatchPending()) ReportJsError(*err);
  }
 private:
  ui::js::Runtime* rt_;
  ui::js::PersistentRef fn_;
};
```

`Touch.onTap` 这类回调被绑成 `std::function<void(const TapDetails&)>`，内部就是 `JsEventCallback`。Element unmount 时回调对象析构，持久句柄释放，避免 JS 闭包泄漏。

---

## 6. 内置控件集（首批）

> 原则：**少而准**，对齐 Flutter 中最高频、布局/绘制最关键的能力。复合控件由 JS 业务侧基于这些原子组合。

### 6.1 RenderObjectWidget（叶子，绑定 RO）
| Widget | 对应 RO | 说明 |
|---|---|---|
| `Box` | `RenderColoredBox / RenderDecoratedBox` | 背景、边框、阴影、圆角、渐变 |
| `ConstrainedBox` | `RenderConstrainedBox` | width/height/min/max |
| `Padding` | `RenderPadding` | EdgeInsets |
| `Align` | `RenderPositionedBox` | Alignment |
| `Transform` | `RenderTransform` | matrix4 |
| `Opacity` | `RenderOpacity` | |
| `ClipRect / ClipRRect / ClipPath` | `RenderClipXxx` | |
| `Image` | `RenderImage` | 图片源、fit、scale |
| `Text` | `RenderParagraph` | TextSpan / 富文本 |
| `CustomPaint` | `RenderCustomPaint` | C++ 业务绘制；JS 暴露 `Path/Paint` HostObject |
| `RepaintBoundary` | `RenderRepaintBoundary` | |
| `Offstage` | `RenderOffstage` | |

### 6.2 多子节点布局
| Widget | RO | 说明 |
|---|---|---|
| `Flex (Row/Column)` | `RenderFlex` | direction/main/cross/spacing |
| `Stack` | `RenderStack` | Positioned 子节点 |
| `Wrap` | `RenderWrap` | |
| `ListBody` | `RenderListBody` | |
| `Scroll`（内嵌 Viewport） | `RenderViewport + RenderSliverList` | 垂直/水平滚动 |
| `GridView`（简化） | `RenderSliverGrid` | |

### 6.3 交互
| Widget | 说明 |
|---|---|
| `Touch` | onTap/onLongPress/onDoubleTap，封装 GestureRecognizer |
| `Pan` | onPanStart/Update/End |
| `Scroll`（同上）| 含手势识别 |
| `Input` | 文本输入；接平台 IME |
| `Focus` | 焦点管理 |

### 6.4 数据/继承
| Widget | 说明 |
|---|---|
| `Inherited<T>` | 等价 InheritedWidget；JS 用 `useInherited(typeId)` |
| `MediaQuery` | 设备/视口信息 |
| `Theme`（轻量） | 提供 color/text style 等基础语义，**不做 material**完整复刻 |

### 6.5 业务用 JS 怎么自定义复合 widget

直接写 JS 函数即可（见 §5.1 `Counter`）。需要 stateful 时用 `useState`/`useReducer`/`useEffect`/`useInherited` —— 完全 React-like 心智。

---

## 7. 动态化协议：脚本交付、热更、安全

### 7.1 产物形态
- **JS 包**：业务 JS（含依赖打包）→ 编译成引擎字节码：
  - quickjs：`JS_WriteObject` 输出 `.qbc`
  - v8：`v8::ScriptCompiler::CachedData` 输出 `.v8bc`
- **资源包**：图片、字体、本地化文案，独立 manifest。
- **包描述**：`bundle.json` 含 `entry`、`version`、`engine`（quickjs/v8）、`min_runtime`、`signature`。

### 7.2 下发与加载
```
Server  ──签名─▶  CDN  ──HTTPS─▶  客户端
                              │
                              ▼
                    本地缓存（按 version + engine 分目录）
                              │
                              ▼
            UiKernel.LoadBundle(path) → Runtime.EvaluateBytecode(...)
                              │
                              ▼
                     mountRoot(jsRootComponent)
```

**回滚**：保留上一可用版本，新版启动失败/崩溃次数超阈值自动回退。

**完整性**：包级 Ed25519 签名校验；字节码版本与运行时版本绑定（不同 engine、不同 v8 版本字节码不通用，必须分发匹配产物）。

### 7.3 热更新粒度
- **整包热更**：下发新 bundle，重建 root element 子树（保留全局状态需业务自管 / 持久化）。
- **子页热更**：每个"页面"是一个独立 root，热更只重建该页 element 子树，其他页不动。
- **不做 Dart 那种 stateful hot reload**：成本高、JS 闭包 + hooks 与重建语义本来就匹配，业务接受"重建 + 状态持久化"心智。

### 7.4 安全
- **沙箱**：JS 默认无 IO/网络/文件访问；要用必须经 `PlatformChannel` 显式调用，宿主侧白名单 + 权限审查。
- **资源限额**：每个 Runtime 配 `memory_limit`（quickjs `JS_SetMemoryLimit`，v8 ResourceConstraints）+ `gc_threshold`，超限触发回收/崩溃可控降级。
- **执行时长**：每帧 build 给定预算（如 8ms），超时打断当帧 build 并 warn（与 Flutter "jank" 同语义）。
- **eval 禁用**：默认禁用 `eval` / `Function` 构造（quickjs 编译选项 / v8 `AllowCodeGenerationFromStrings(false)`）。

---

## 8. 对外 ABI（嵌入与平台层）

### 8.1 C 头 ABI（替代 Flutter embedder API）

```c
// ui_kernel.h —— 稳定 C ABI，平台层（iOS/Android/桌面/嵌入式宿主）只依赖这个
typedef struct UiKernel UiKernel;
typedef struct UiSurface UiSurface;
typedef struct UiBundle UiBundle;

typedef struct {
  const char* js_engine;       // "quickjs" | "v8"
  size_t      js_memory_limit;
  void*       platform_message_handler;   // 函数指针
  /* ... */
} UiKernelConfig;

UiKernel*  UiKernelCreate(const UiKernelConfig*);
void       UiKernelDestroy(UiKernel*);

UiSurface* UiKernelAttachSurface(UiKernel*, void* native_layer, int w, int h, float dpr);
void       UiKernelDetachSurface(UiKernel*, UiSurface*);
void       UiKernelOnVsync(UiKernel*, int64_t frame_time_nanos);
void       UiKernelDispatchPointer(UiKernel*, const UiPointerData*, size_t n);

UiBundle*  UiKernelLoadBundle(UiKernel*, const char* path);
void       UiKernelMountRoot(UiKernel*, UiBundle*, const char* entry_export);
void       UiKernelReplaceBundle(UiKernel*, UiBundle* new_bundle);  // 热更
```

### 8.2 平台适配
- **iOS**：UIView 子类承载 `UiSurface`；CADisplayLink → `UiKernelOnVsync`；UITouch → `UiPointerData`。
- **Android**：SurfaceView/TextureView；Choreographer → vsync；MotionEvent → pointer。
- **桌面**：GLFW / SDL / 各 OS 原生 surface，自管 vsync（或交由合成器）。
- **嵌入式 / 自定义**：直接接 EGL/Vulkan surface + 自有 vsync 时钟。

### 8.3 PlatformChannel（替代 MethodChannel）

完全 C++ 协议：

```cpp
class PlatformChannel {
 public:
  using Handler = std::function<void(BytesRef payload, ResponseFn reply)>;
  void RegisterMethod(StringRef name, Handler);
  void InvokeMethod(StringRef name, BytesRef payload, ResponseFn reply);
};
```

- 编码：`flexbuffers`（FlatBuffers 子集，二进制、零拷贝、跨语言）。
- JS 侧：`platform.invoke(name, payload).then(reply => ...)` —— binding 包成 Promise。
- 原生侧：宿主用 C++ 注册 handler；iOS/Android 提供 Obj-C/Java/Kotlin 包装层，但运行期协议是 C++。

---

## 9. 文本、图片、字体子系统

### 9.1 文本
- 排版：直接用 `SkParagraph`（Skia 自带，已是 engine 默认排版引擎）。
- 平台字体获取：`SkFontMgr` 平台实现（iOS Core Text, Android NDK FontConfig）。
- 富文本：JS 侧 `Text({ spans: [...] })`，binding 转 `TextSpanTree`（C++）→ `SkParagraphBuilder`。

### 9.2 图片
- 解码：直接用 engine 现有 `ImageDecoder` 的 C++ 部分（去掉 dart binding）。
- 缓存：内核内置 LRU `ImageCache`（接口同 `ui::ImageCache` 心智）。
- JS 侧 `Image({ src: 'asset://x.png' | 'http://...' | 'memory://hash' })`。

### 9.3 字体
- 启动期可注册资源包内字体（OTF/TTF）到 `SkFontMgr`。

---

## 10. 调试与诊断

### 10.1 JS 侧
- **quickjs**：自带 `console.log`、source map（手实现）；inspector 由我们自写一个最小的 CDP 子集（domains: Runtime/Debugger/Console），通过 WebSocket 暴露给 Chrome DevTools。
- **v8**：直接接入 `v8-inspector`，CDP 全功能。

### 10.2 C++ 侧
- **DiagnosticTree**：替代 Dart `DiagnosticableTree`，每个 `Element / RenderObject` 实现 `DescribeTree(StringSink&)`。
- **Inspector 协议**：单独 WebSocket 协议（自定义）暴露 element 树、RO 树、约束/尺寸/重绘区域、layer 树。配套桌面工具或 Web 工具消费。
- **Timeline**：埋点直接复用 engine 已有的 `fml::tracing` / Perfetto，build/layout/paint/raster 各阶段可见。

### 10.3 崩溃与日志
- C++ 侧 `AddressSanitizer / UBSan` 全程开（debug+canary）。
- JS 异常：`Runtime::CatchPending` 捕获 → 上报（含 source map 还原栈）。
- 内核断言失败：写 minidump + 上报 + 快速降级（卸载当前 bundle、回退到上一版本或原生兜底页）。

---

## 11. 构建与工程

### 11.1 仓库结构（fork engine 后）
```
ui-kernel/
  core/              # WidgetDesc, Element, BuildOwner, RenderObject, ...
  rendering/         # 各 RenderObject 实现
  widgets/           # 内置控件（Box/Flex/Text/...）
  gestures/          # GestureArena 等
  animation/         # Ticker/Curve/Tween
  text/              # 排版封装
  asset/             # ImageCache 等
  platform_channel/  # PlatformChannel
  engine_bindings/   # 接 flow / DisplayList / VsyncWaiter
js-runtime/
  api/               # ui::js::Runtime 抽象头
  quickjs_impl/      # quickjs 适配
  v8_impl/           # v8 适配
  bindings/          # 内核 ↔ JS 的 binding 实现（Widget 工厂、Hooks、Channel）
embedder/
  c_api/             # ui_kernel.h（C ABI）
  ios/  android/  desktop/  embedded/
toolchain/
  bundler/           # JS 业务打包工具：esbuild/swc + 字节码生成
  inspector/         # 调试协议服务
third_party/
  engine/            # fork 自 flutter/engine（已删 Dart 部分）
  quickjs/  v8/  skia/  ...
```

### 11.2 构建系统
- 沿用 engine 的 GN/Ninja 体系（已有，省事）；新增 `js_engine = "quickjs" | "v8"` 顶层参数。
- 业务 JS 打包工具链（`toolchain/bundler/`）：
  - 解析 + tree shaking：esbuild 或 swc。
  - 字节码生成：调 quickjs 的 `qjsc` / v8 的 snapshot 工具，产物按 engine 分目录。
  - 资源打包 + manifest + 签名。

### 11.3 产物体积参考（粗估）
- 内核（不含 JS 引擎）：~3–5 MB
- quickjs：~700 KB–1 MB
- Skia/Impeller + flow：~6–10 MB（与现 Flutter 持平）
- v8（如选）：~10 MB+（注意预算）
- 业务 JS bundle：随业务（典型电商首页 100–500 KB 字节码）

---

## 12. 路线图

| 阶段 | 时长 | 目标 | 验收 |
|---|---|---|---|
| **0. PoC** | 1.5 月 | 砍掉 Dart 后能从 vsync 到出图：C++ `RenderView + RenderColoredBox + RenderPadding`，挂 LayerTree，单平台（macOS/Linux）画出方块 | 真机/模拟器出图，FPS 稳定 |
| **1. RO 层骨架** | 3 月 | RenderBox/Flex/Stack/Padding/Decorated/Clip/Transform/Opacity/Paragraph/Image；layout/paint/hitTest 闭环 | 能拼出复杂静态页（不含动效） |
| **2. Element 内核** | 4 月 | Element/BuildOwner/diff/InheritedScope/State/Hooks/GlobalKey 重定位 | 内置 widget 拼装的页面交互正常（无 JS） |
| **3. 手势 + 调度 + 动画** | 2 月 | GestureArena、vsync 调度、Ticker、Curve、AnimationController | 手势/动画 demo 全跑通 |
| **4. JS Runtime 抽象 + quickjs 适配** | 2 月 | `ui::js::Runtime` 抽象层 + quickjs 实现；bytecode 缓存；microtask | 能跑独立 JS 脚本 |
| **5. Widget JS Binding** | 3 月 | 内置控件全 binding；useState/useEffect/useInherited；事件回调；ElementHandle 校验 | 能用 JS 写出 §5.1 样例并稳定运行 |
| **6. 文本 / 图片 / 资源 / Channel** | 2 月 | SkParagraph 接入、ImageCache、字体注册、PlatformChannel 端到端 | 能加载远程图片、调原生能力 |
| **7. v8 适配** | 1.5 月 | 复用抽象层，v8 实现；inspector 接入 | quickjs / v8 切换业务无感 |
| **8. 动态化 + 调试 + Inspector** | 2 月 | Bundle 协议、签名、热更、JS Inspector、UI Inspector | 远程下发 bundle 并热更成功 |
| **9. 平台 embedder 完整化** | 2 月 | iOS/Android/桌面 embedder 完整；输入法、可访问性最小集 | 三平台业务可用 |
| **10. 性能、稳定性、上线** | 持续 | benchmark、内存压测、ASAN/LSAN、灰度发布 | 关键页面 vs Flutter 基线持平或更优 |

**总量**：约 **24–26 月**（团队 8–12 人专职）。

---

## 13. 关键风险与对策

| 风险 | 说明 | 对策 |
|---|---|---|
| **JS 跨边界开销** | 每帧 setState 重建子树要跨 C++↔JS 多次 | 1) Hooks/build 在同一线程同步执行避免上下文切换；2) `WidgetDesc` 物化延迟到 commit；3) 静态结构子树缓存（key 稳定时跳过 marshal）|
| **JS GC 与 C++ 生命周期** | 句柄悬垂、闭包泄漏 | ElementHandle id+gen 校验；持久句柄随 Element unmount 显式释放；Finalizer 仅作兜底 |
| **生态断裂** | 没 material、没 pub | 接受现实；自建基础控件 + 业务沉淀；预留 CustomPaint 兜底 |
| **fork 维护成本** | engine 跟随上游成本 | 严守 §2 边界：不改 engine 内部，只删 Dart；Skia/Impeller 跟随上游升级；定期 rebase |
| **v8 体积** | v8 比 Dart VM 还大 | 默认 quickjs；v8 仅作可选；体积敏感产物禁用 v8 |
| **调试体验** | 没 hot reload，开发者抗拒 | 子页热更 + JS inspector + 快速重建链；强调"重建 + 持久化"心智 |
| **文本 i18n / RTL / 复杂排版** | 难点集中 | SkParagraph 已覆盖；前期不自造，遇 bug 上报 Skia |
| **可访问性 / 输入法** | 平台细节多 | 阶段 9 专门攻坚；先覆盖 iOS VoiceOver / Android TalkBack 主路径 |
| **首屏启动** | JS 解释 + 字节码加载耗时 | 预编译字节码 + 启动期 mmap + 关键路径 C++ 兜底页 |

---

## 14. 决策小结

1. **架构定型**：fork engine → 删 Dart → C++ 重建三棵树 → 接 flow/DisplayList → 上层挂 JSI-Like Runtime（quickjs/v8 二选一编译）。
2. **JS 角色**：业务动态层；通过 HostObject + ElementHandle(id,gen) 直通 C++；不依赖消息桥。
3. **生命周期主权**：永远在 C++；JS 持有的全是带校验弱句柄；持久句柄随 Element unmount 显式释放。
4. **首选引擎**：quickjs（与"去 runtime 省体积"一致）；v8 仅在重 JS 业务/已有 v8 宿主时启用。
5. **生态取舍**：放弃 material / pub；自建 20–40 个基础控件；上层主题/复合控件由业务/SDK 沉淀。
6. **路线**：分 11 个阶段（含 PoC 与上线收口），约 24–26 月；阶段 0/1 即可验证最大未知（出图）。
7. **不做 hot reload**：用"子页热更 + 状态持久化"代替，符合 React 心智。

---

## 附录 A：关键代码锚点

**engine 复用（保留）**：
- `engine/src/flutter/flow/layers/layer_tree.h` —— `LayerTree`
- `engine/src/flutter/display_list/dl_builder.h` —— `DisplayListBuilder`
- `engine/src/flutter/display_list/display_list.h` —— `DisplayList`
- `engine/src/flutter/flow/surface.h` / `compositor_context.h` —— 合成入口
- `engine/src/flutter/shell/common/vsync_waiter.h` —— vsync
- `engine/src/flutter/lib/ui/window/pointer_data.h` —— 输入（数据结构需抽离 lib/ui）
- `engine/src/flutter/lib/ui/painting/image_decoder.*` —— 图片解码 C++ 主体

**engine 待删除（含 Dart 依赖）**：
- `engine/src/flutter/runtime/` —— Dart VM 集成
- `engine/src/flutter/lib/ui/**.dart` + `*_natives.cc` —— dart:ui Dart 入口及胶水
- `engine/src/flutter/shell/platform/embedder/` —— 标准 embedder API（强依赖 Dart snapshot）
- `packages/flutter*` —— Dart framework / tools / test

**对照参考（设计灵感来源）**：
- `engine/src/flutter/skwasm/` —— 已有的"绕过 embedder、直接接 flow 层"实现，本方案 embedder 设计参考它
- React Native JSI（Hermes/HostObject 模式）—— `ui::js::Runtime` 抽象层设计参考它

---

## 15. PoC 阶段 0：先把"砍 Dart 出一帧"打通

> 整套方案的最大未知 = **能不能在不依赖 Dart runtime 的前提下，复用 engine 的 flow / DisplayList / Rasterizer 这层 C++ 底座，独立驱动出一帧画面到 macOS 窗口**。
> 这个未知不验证，后面 8–12 人 × 24 月的投入全是空中楼阁。
> PoC 目标就一件事：**用最小代码量，证明这条路通**。其它（JS 引擎、三棵树、动态化协议）阶段 0 一律不碰。

### 15.1 PoC 一句话目标

**1.5 个月，2 人，在 macOS 上跑出一个窗口，里面画一个红色矩形 + "Hello" 文字 + 一个能响应点击变色的方块。整个进程内：零 Dart VM、零 dart 字节码、零 `RuntimeController`、零 `embedder.h` 的 `FlutterEngineRun` 调用。**

达成这个目标 = 验证 4 件事：

| 验证点 | 说明 |
|---|---|
| **V1 渲染底座可独立** | `flow::LayerTree` + `DisplayListBuilder` + `Rasterizer` 可以脱离 `shell::Engine` / `RuntimeController` 直接驱动 |
| **V2 vsync 可独立接** | 不通过 `shell::Shell`，用 `CVDisplayLink`（macOS）直接喂 `Rasterizer::Draw` |
| **V3 Surface 可独立挂** | Metal `CAMetalLayer` 直接对接 `EmbedderSurfaceMetalSkia` 等价物（或更底层的 `Surface` 实现） |
| **V4 输入可独立路由** | macOS NSEvent → 自写一个最小 hit-test，证明事件链不依赖 `PlatformView`/`Engine` |

> 任何一项不通过，都要在 PoC 内当场修复或承认架构改动量；不要把问题留到阶段 1。

### 15.2 PoC 不做什么（关键约束）

为了保证 1.5 月可交付，**以下东西 PoC 阶段 0 一律不做**：

- ❌ 不接 quickjs、不接 v8、不写任何 binding。业务硬编码 C++。
- ❌ 不实现 Widget/Element/RenderObject 三棵树。直接手搓 `DisplayListBuilder` 调用。
- ❌ 不做 layout 算法（Flex/Stack 都不做）。坐标硬编码。
- ❌ 不做手势 arena。点击只用最朴素的 AABB hit-test。
- ❌ 不做 GestureRecognizer / Animation / Ticker / Text shaping 复杂化。文字用 `SkParagraphBuilder` 最简调用。
- ❌ 不跨平台。**只 macOS**（开发机 = 目标机，调试链最短）。
- ❌ 不动 BUILD.gn 的全局结构。新代码塞在 `engine/src/flutter/poc_cpp_kernel/` 子目录，独立 `BUILD.gn`，走 `gn gen` 单独编译。

### 15.3 PoC 物理目录

```
engine/src/flutter/poc_cpp_kernel/        # 全部新代码在此
├── BUILD.gn                              # 单独 target，不污染主图
├── README.md
├── app/
│   ├── main.mm                           # macOS 入口：NSApplication + NSWindow + CAMetalLayer
│   └── poc_scene.cc                      # PoC 业务：硬编码 3 个"控件"
├── kernel_min/                           # 极简 C++ 内核（PoC 专用，不是最终设计）
│   ├── poc_node.h / .cc                  # 一个简化"节点"：{ rect, kind, props, on_tap }
│   ├── poc_painter.h / .cc               # 节点 → DisplayListBuilder
│   ├── poc_hittest.h / .cc               # AABB 点击命中
│   └── poc_pipeline.h / .cc              # 调度：input → mark dirty → repaint → submit
├── platform_mac/
│   ├── metal_surface.h / .mm             # 复用 EmbedderSurfaceMetalSkia 的最小骨架
│   ├── vsync_cvdisplaylink.h / .mm       # CVDisplayLink → 触发 PocPipeline::Tick
│   └── input_nsview.h / .mm              # NSEvent → PocPipeline::OnPointer
└── third_party_glue/
    └── rasterizer_adapter.h / .cc        # 直接 new flutter::Rasterizer，绕开 Shell
```

> PoC 完成后这个目录会**整体废弃**（不是阶段 1 的基础），它的价值只在于"证明可行"。阶段 1 重写时会按 §3 设计正式建 `ui-kernel/`。

### 15.4 PoC 渲染主链路（一定要走通的代码路径）

```
[CVDisplayLink callback (raster thread)]
        │
        ▼
PocPipeline::Tick(now)
        │  if dirty:
        ▼
PocPainter::PaintScene(scene_root) ──► DisplayListBuilder
        │                              │
        │                              ▼
        │                          DisplayList (sk_sp)
        ▼
LayerTree (1 个 PictureLayer 包住整张 DisplayList)        ◄── 阶段 0 不分 layer
        │
        ▼
flutter::Rasterizer::Draw(layer_tree)                     ◄── 关键复用点
        │
        ▼
EmbedderSurfaceMetalSkia 的等价 Surface ──► CAMetalLayer ──► 屏幕
```

**3 个关键验证锚点（PoC 必须 hands-on 触碰）**：

1. **`flutter::Rasterizer` 的最小构造**
   `engine/src/flutter/shell/common/rasterizer.h` 里 `Rasterizer` 接受 `Delegate&`。需要写一个 `PocRasterizerDelegate : Rasterizer::Delegate`，stub 出所有虚函数（大部分返回默认值），重点实现 `GetTaskRunners()` 和 `OnFrameRasterized()`。
   **这一步若卡住，说明 Rasterizer 与 Shell 的耦合比预想深，需要重新评估 §2.2 里"砍 shell 保 rasterizer"的可行性。**

2. **Surface 直挂 CAMetalLayer**
   参考 `engine/src/flutter/shell/platform/embedder/embedder_surface_metal_skia.mm` 的内部组装方式，**复制一份精简版**到 PoC，跳过 embedder 的 `FlutterMetalRendererConfig` 那层 C 接口。验证可以直接 `new` 这个 surface 喂给 `Rasterizer`。

3. **DisplayListBuilder → 真实可见像素**
   ```cpp
   DisplayListBuilder b(SkRect::MakeWH(800, 600));
   b.DrawRect(SkRect::MakeXYWH(100, 100, 200, 150),
              DlPaint().setColor(DlColor::kRed()));
   sk_sp<DisplayList> dl = b.Build();
   ```
   能在窗口里看到红方块 = V1+V3 同时通过。

### 15.5 PoC 的极简"控件"模型

**不要用三棵树**。PoC 用 1 棵树、1 个节点类型：

```cpp
// kernel_min/poc_node.h
struct PocNode {
  enum Kind { kBox, kText, kTapBox };
  Kind kind;
  SkRect rect;                      // 绝对坐标，不做 layout
  uint32_t color;                   // kBox / kTapBox 的填充色
  std::string text;                 // kText 的文字
  std::function<void()> on_tap;     // kTapBox 的回调（点击时切换 color）
  std::vector<std::unique_ptr<PocNode>> children;  // 仅用于组合，不参与布局
};
```

PoC 业务（`poc_scene.cc`）硬编码 3 个节点：

```cpp
auto root = std::make_unique<PocNode>(...);
root->children.push_back(MakeBox({100,100,300,250}, 0xFFFF0000));   // 红方块（验证 V1）
root->children.push_back(MakeText({100,300}, "Hello PoC"));         // 文字（验证 V1+text）
auto tap = MakeTapBox({400,100,600,250}, 0xFF00AA00);
tap->on_tap = [&] { tap->color ^= 0x00FFFFFF; pipeline.MarkDirty(); };
root->children.push_back(std::move(tap));                           // 点击变色（验证 V4）
```

`PocPainter::PaintScene` 就是一个 DFS：每种 kind 调对应的 `DlCanvas` API。**不超过 200 行代码**。

### 15.6 时间盒（1.5 个月，2 人）

| 周 | 里程碑 | 产出 | 通过标准 |
|---|---|---|---|
| **W1** | 摸清 Rasterizer 真实依赖 | 一份"`Rasterizer::Delegate` 接口拆解笔记 + stub 草稿" | 能列出每个虚函数是否真的需要 / 能否返默认 |
| **W2** | 空窗口 + Metal surface | macOS 窗口能起来，CAMetalLayer 接到自写 Surface，每帧 clear 成纯色 | 屏幕上能看到稳定 60fps 的纯色 |
| **W3** | 接通 DisplayList | `Rasterizer::Draw(layer_tree)` 跑通，画出红方块 | **V1 + V3 通过** |
| **W4** | CVDisplayLink 驱动 + 多帧 | 节点 color 动画切换，每帧重画 | **V2 通过**；无掉帧 |
| **W5** | NSEvent → 点击变色 | 点击 tap_box，颜色翻转 | **V4 通过** |
| **W6** | 加文字 + 收尾 | SkParagraph 渲染 "Hello PoC"；写 PoC 报告 | 4 项验证全过；文档归档 |

**任意一周延期 > 3 天 → 立即开评估会**：要么调整目标（例如砍掉文字），要么承认底层耦合超预期、阶段 1 需要追加预算。**不要默默拖**——PoC 的全部价值是"早发现"。

### 15.7 验收清单（PoC 完成 = 以下全部 YES）

- [ ] 进程内无 `libdart.so` / `libflutter_engine.dylib` 的 Dart 部分（用 `nm` / `otool -L` 验证）
- [ ] 不调用 `FlutterEngineRun` / `FlutterEngineInitialize` 任何 embedder C API
- [ ] 不构造 `shell::Shell` / `shell::Engine` / `RuntimeController` 任何对象
- [ ] **直接构造 `flutter::Rasterizer`**，能 `Draw` 出 `LayerTree`
- [ ] macOS 窗口稳定 60fps（用 `CFAbsoluteTimeGetCurrent` 自测帧间隔）
- [ ] 红方块 / 文字 / 可点击方块全部可见且交互正确
- [ ] PoC 二进制大小 < 30MB（验证"砍 Dart 省体积"假设方向正确，不是绝对值）
- [ ] 一份 ≤ 10 页的 **PoC 复盘文档**，明确写出：
  - Rasterizer / Surface / DisplayList 的真实复用成本
  - 哪些 engine 头文件需要"复制改造"而非"原样 include"（这是阶段 1 的工作量基线）
  - 阶段 1 是否调整：保留全部三棵树设计 / 砍掉部分 / 重新评估

### 15.8 PoC 通过之后，阶段 1 第一件事

**不是写 Widget/Element/RenderObject**，而是：

1. 把 PoC 里的 `rasterizer_adapter.h` 抽成正式的 `ui-kernel/platform/rasterizer_host.h`——这是后续所有渲染的入口。
2. 把 PoC 里的 `metal_surface` 推广到 iOS / Android（GL/Vulkan）平台抽象，确认 §6 的平台层切面可以对齐。
3. 这两步做完，再开始 §3 的三棵树。**先确保支点稳，再盖楼。**

### 15.9 PoC 失败的两种处理

PoC 失败有两种：

- **"Rasterizer 太耦合 Shell" 类失败**：说明 §2.2 的复用边界要往下挪——可能要绕过 `Rasterizer` 直接用 `flow::CompositorContext` + `Surface`。这意味着阶段 1 多 1–2 月，但方案大方向不变。
- **"Skia / Impeller 没有 Dart-free 调用路径" 类失败**（极小概率）：说明 engine 的 C++ 底座本身不再独立——这种情况方案需要回到旧文档的"保留 Dart 外壳"渐进路线。**这是 PoC 唯一可能否决整套方案的结果，必须诚实面对。**

> 写在最后：PoC 不是 demo，PoC 是**架构假设的真伪检验**。1.5 个月、6 人周的投入，换的是后面 24 个月不踩坑的资格。**不要省这一步。**
