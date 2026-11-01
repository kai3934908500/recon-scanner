// SPDX-License-Identifier: BSD-3-Clause
//
// CaptureStateBridge.mm — 采集状态机桥接实现（Objective-C++）。
//
// 直接复用 platform/common/src/capture_state.cpp 的 C++ 实现，保证 iOS 与
// Android 两端状态迁移规则**同源**（铁律：同一逻辑写两遍必然漂移）。

#import "CaptureStateBridge.h"

#include "recon_platform/capture_state.h"

using recon::platform::CaptureEvent;
using recon::platform::CaptureState;
using recon::platform::CaptureStateMachine;
using recon::platform::FailureStage;

// 把 C++ 状态机实例作为不透明指针持有，避免在公共头里引入 C++ 类型。
@interface ReconCaptureStateMachine () {
    void* _native;
}
@end

@implementation ReconCaptureStateMachine

+ (instancetype)shared {
    static ReconCaptureStateMachine* instance = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ instance = [[ReconCaptureStateMachine alloc] init]; });
    return instance;
}

// 状态机实例持有为 C++ 对象。用 ivar 存指针以避免在 ObjC 头里引入 C++ 类型。
- (instancetype)init {
    self = [super init];
    if (self) {
        _native = new CaptureStateMachine();
    }
    return self;
}

- (void)dealloc {
    delete static_cast<CaptureStateMachine*>(_native);
    _native = nullptr;
}

- (ReconCaptureState)applyEvent:(ReconCaptureEvent)event {
    auto* m = static_cast<CaptureStateMachine*>(_native);
    const auto r = m->Apply(static_cast<CaptureEvent>(event));
    return static_cast<ReconCaptureState>(r.next);
}

- (ReconCaptureState)currentState {
    return static_cast<ReconCaptureState>(static_cast<CaptureStateMachine*>(_native)->state());
}

- (ReconFailureStage)failureStage {
    return static_cast<ReconFailureStage>(static_cast<CaptureStateMachine*>(_native)->failure_stage());
}

- (void)setFailureStage:(ReconFailureStage)stage {
    static_cast<CaptureStateMachine*>(_native)->set_failure_stage(static_cast<FailureStage>(stage));
}

- (BOOL)isPaused {
    return static_cast<CaptureStateMachine*>(_native)->paused() ? YES : NO;
}

- (void)clearPaused {
    static_cast<CaptureStateMachine*>(_native)->clear_paused();
}

@end
