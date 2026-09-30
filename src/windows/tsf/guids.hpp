#pragma once
#include <windows.h>
namespace rime_ime::tsf {
inline constexpr CLSID service_id = {
    0x1a5d0370, 0x40a3, 0x4865, {0x90, 0xa6, 0x32, 0x4e, 0x1b, 0xb1, 0x85, 0x5b}};
inline constexpr GUID profile_id = {
    0x80b722f7, 0xfa50, 0x4d8e, {0xa0, 0x84, 0xe1, 0x2a, 0x5b, 0x90, 0x65, 0x85}};
inline constexpr GUID language_bar_id = {
    0x6d2f4aae, 0xe536, 0x459f, {0x9b, 0x2f, 0xb7, 0x9a, 0xde, 0x89, 0xf8, 0x04}};
inline constexpr LANGID language_id = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
inline constexpr wchar_t display_name[] = L"R-IME";
inline constexpr wchar_t service_guid[] = L"{1A5D0370-40A3-4865-90A6-324E1BB1855B}";
inline constexpr wchar_t profile_guid[] = L"{80B722F7-FA50-4D8E-A084-E12A5B906585}";
} // namespace rime_ime::tsf
