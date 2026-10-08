// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

// Placeholder until the save-image codec lands (stream savecodec replaces this file).

#include "common/logging.h"
#include "core/file_sys/save_image/save_image.h"

namespace FileSys::SaveImage {

std::optional<ExportedImage> ExportSaveImage(const VirtualDir&, const SaveGeometry&) {
    LOG_WARNING(Service_FS, "save image: no codec in this build");
    return std::nullopt;
}

bool ImportSaveImage(const VirtualFile&, const ImageParameters&, const SaveGeometry&,
                     const VirtualDir&) {
    LOG_WARNING(Service_FS, "save image: no codec in this build");
    return false;
}

} // namespace FileSys::SaveImage
