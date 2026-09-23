#pragma once

#include <string>
#include <vector>

namespace eh::file_browser {

struct AppState;

struct CompressFormat {
  std::string label;
  std::string extension;
};

constexpr int kNumCompressFormats = 7;
extern const CompressFormat kCompressFormats[kNumCompressFormats];

std::string format_compress_cmd(const std::vector<std::string>& source_paths,
                                 const std::string& archive_path,
                                 int format_idx, int level, int threads);

// Thread choices for the dialog (0 = Auto). Powers of two plus the detected
// hardware count as the max, so Auto and the top button both mean "all cores".
std::vector<int> compress_thread_options();
// Hardware threads, always >= 1 (falls back to 4 when undetectable).
unsigned compress_hw_threads();
// Resolve 0=Auto to hardware threads, clamped to >= 1.
int compress_effective_threads(int threads);
// Button width for the threads row given the option count (fits 380px).
int compress_thread_btn_w(int count);

bool is_archive_extension(const std::string& path);
bool is_iso_image(const std::string& path);
std::string format_extract_cmd(const std::string& archive_path,
                                const std::string& dest_dir);
std::string default_extract_dir(const std::string& archive_path);

void check_compress_tool_availability(AppState& app);
void execute_compress_async(AppState& app);
void execute_extract_async(AppState& app, const std::string& archive_path,
                            const std::string& dest_dir);
void execute_extract_with_password(AppState& app, const std::string& archive_path,
                                   const std::string& dest_dir,
                                   const std::string& password);
bool archive_is_encrypted(const std::string& archive_path);
void show_password_dialog(AppState& app, const std::string& archive_path,
                           const std::string& dest_dir);

} // namespace eh::file_browser
