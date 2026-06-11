#include "dialog/file_chooser_dialog.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace eh::dialog {

FileChooserDialog::FileChooserDialog(Mode mode, const char* title,
                                      const std::string& accept_type, bool multiple)
  : DialogBase(800, 600, title),
    mode_(mode), accept_type_(accept_type), multiple_(multiple) {}

FileChooserDialog::~FileChooserDialog() = default;

void FileChooserDialog::draw(cairo_t*, int, int) {}
void FileChooserDialog::on_click(int, int, int) {}
void FileChooserDialog::on_pointer_move(int, int) {}
void FileChooserDialog::on_scroll(int, int, double, double) {}
void FileChooserDialog::on_key(xkb_keysym_t, uint32_t, const char*, int) {}

void FileChooserDialog::load_dir(const std::string&) {}
void FileChooserDialog::go_up() {}
void FileChooserDialog::open_selected() {}
void FileChooserDialog::rebuild_visible() {}
void FileChooserDialog::toggle_view() {}
void FileChooserDialog::toggle_hidden() {}
bool FileChooserDialog::entry_hidden(int) const { return false; }
int  FileChooserDialog::entry_at(int, int) const { return -1; }
void FileChooserDialog::draw_grid(cairo_t*, int, int) {}
int  FileChooserDialog::grid_cols() const { return 1; }
int  FileChooserDialog::grid_rows() const { return 0; }
int  FileChooserDialog::grid_cell_h() const { return 100; }
cairo_surface_t* FileChooserDialog::get_thumbnail(const std::string&, int) { return nullptr; }

std::string file_uri_to_local_path(const std::string& uri) {
  if (uri.size() > 7 && uri.substr(0, 7) == "file://") return uri.substr(7);
  return uri;
}

bool show_native_folder_picker(std::string* out_path) {
  // Use kdialog / zenity as fallback
  std::string cmd = "zenity --file-selection --directory 2>/dev/null";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return false;
  char buf[4096];
  if (fgets(buf, sizeof(buf), f)) {
    std::string path(buf);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    pclose(f);
    if (!path.empty()) {
      *out_path = path;
      return true;
    }
  }
  pclose(f);
  return false;
}

}
