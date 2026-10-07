#pragma once
#include <msctf.h>

namespace rime_ime::tsf {
HRESULT input_attribute(REFGUID guid, ITfDisplayAttributeInfo **info);
HRESULT enumerate_attributes(IEnumTfDisplayAttributeInfo **attributes);
} // namespace rime_ime::tsf
