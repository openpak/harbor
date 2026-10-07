// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/service/bcat/news/overwrite_event_holder.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/bcat/news/news_shared.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::News {

IOverwriteEventHolder::IOverwriteEventHolder(Core::System& system_, std::shared_ptr<NewsShared> shared_)
    : ServiceFramework{system_, "IOverwriteEventHolder"}, shared{std::move(shared_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, D<&IOverwriteEventHolder::Get>, "Get"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IOverwriteEventHolder::~IOverwriteEventHolder() = default;

Result IOverwriteEventHolder::Get(OutCopyHandle<Kernel::KReadableEvent> out_event) {
    *out_event = &shared->overwrite->GetReadableEvent();
    R_SUCCEED();
}

} // namespace Service::News
