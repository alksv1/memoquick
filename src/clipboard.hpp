#pragma once
#include "core.hpp"
#include <oleidl.h>

namespace mq {
struct ClipboardAttachment {
    std::string filename, type, bytes;
    bool operator==(const ClipboardAttachment&) const = default;
};
// Reads an explicit paste, never monitors or modifies the system clipboard.
// Empty means ordinary text/unsupported data: let the native editor handle it.
std::vector<ClipboardAttachment> clipboardAttachments(IDataObject& source);
} // namespace mq
