// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file text_editor.hpp
 * @brief A small full-screen text editor for the bootloader console: edits a dynamic
 * `reloco::string` (heap, allocator-backed, grows as the user types) over a VT100/ANSI terminal.
 *
 * Three layers, so each can be used or tested alone:
 *  - `key_decoder` (sans-IO): turns received bytes into `key_event`s (printable characters, Enter,
 *    Backspace, Delete, arrows, Home/End, PageUp/PageDown and the Ctrl commands below).
 *  - `text_editor` (sans-IO apart from `render`): the cursor, the edit operations on the string,
 *    scrolling, and `render()` which redraws the visible part on a `hw::uart_ref`. C++17 compatible.
 *  - `edit_text()` (C++20): a coroutine that runs the editor on a `bootldr::scheduler`, polling the
 *    UART and yielding between keys so other tasks (netstack, ...) keep running.
 *
 * Keys: arrows/Home/End/PageUp/PageDown (also Ctrl-A = Home, Ctrl-E = End), Backspace, Delete,
 * Enter, Tab (spaces to the next tab stop), Ctrl-K (kill to end of line), Ctrl-L (redraw),
 * Ctrl-S (save and quit), Ctrl-X (quit without saving; asks once more if there are changes).
 *
 * ```cpp
 * // The text to edit lives on the heap in a reloco::string, e.g. a boot script.
 * reloco::string script(sched.allocator());
 * (void)script.try_assign("tftp kernel.bin 0x80000\ngo 0x80000\n");
 *
 * // A command handler (or any task) can then co_await the editor; the arguments are the
 * // scheduler that runs it, the console and the text.
 * structo::bootldr::editor_options opt;     // 24 rows x 80 columns, tab width 2 by default
 * auto saved = co_await structo::bootldr::edit_text(sched, uart, script, opt);
 * if (saved && *saved) {
 *   // true: the user pressed Ctrl-S, `script` holds the new text
 * } else {
 *   // false: cancelled, `script` is unchanged; an error means the UART or the allocator failed
 * }
 * ```
 */

#include <structo/bootldr/scheduler.hpp>
#include <structo/hw/uart_ref.hpp>

#include <microfmt/microfmt.hpp>

#include <reloco/error.hpp>
#include <reloco/string.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::bootldr {

/** @brief What a key press means to the editor. */
enum class key_code : std::uint8_t {
  none, ///< byte consumed, no key completed yet (or ignored)
  character,
  enter,
  tab,
  backspace,
  del,
  left,
  right,
  up,
  down,
  home,
  end,
  page_up,
  page_down,
  save,      ///< Ctrl-S
  cancel,    ///< Ctrl-X
  kill_line, ///< Ctrl-K
  redraw     ///< Ctrl-L
};

struct key_event {
  key_code code = key_code::none;
  char ch = 0; ///< the character for `key_code::character`
};

/** @brief Incremental decoder for terminal input, including `ESC [ ...` and `ESC O ...` sequences. */
class key_decoder {
public:
  /** @brief Feeds one received byte; returns the finished key or `key_code::none`. */
  key_event feed(std::uint8_t b) noexcept {
    const bool after_cr = after_cr_;
    after_cr_ = false;
    switch (state_) {
    case state::ground:
      return ground(b, after_cr);
    case state::escape:
      if (b == '[') {
        state_ = state::csi;
        param_ = 0;
      } else if (b == 'O') {
        state_ = state::ss3;
      } else if (b != 0x1b) {
        state_ = state::ground; // unknown Alt-sequence: dropped
      }
      return {};
    case state::ss3:
      state_ = state::ground;
      return final_byte(b);
    case state::csi:
      if (b >= '0' && b <= '9') {
        param_ = param_ * 10 + (b - '0');
        if (param_ > 1000)
          param_ = 1000;
        return {};
      }
      if (b == ';')
        return {}; // modifiers are ignored
      state_ = state::ground;
      if (b == '~')
        return tilde(param_);
      return final_byte(b);
    }
    return {};
  }

private:
  enum class state : std::uint8_t { ground, escape, csi, ss3 };

  static key_event make(key_code c) noexcept { return key_event{c, 0}; }

  key_event ground(std::uint8_t b, bool after_cr) noexcept {
    if (b == 0x1b) {
      state_ = state::escape;
      return {};
    }
    if (b == '\r') {
      after_cr_ = true;
      return make(key_code::enter);
    }
    if (b == '\n')
      return after_cr ? key_event{} : make(key_code::enter); // CRLF is one Enter
    if (b == 0x7f || b == 0x08)
      return make(key_code::backspace);
    if (b == '\t')
      return make(key_code::tab);
    switch (b) {
    case 0x01:
      return make(key_code::home);
    case 0x05:
      return make(key_code::end);
    case 0x0b:
      return make(key_code::kill_line);
    case 0x0c:
      return make(key_code::redraw);
    case 0x13:
      return make(key_code::save);
    case 0x18:
      return make(key_code::cancel);
    default:
      break;
    }
    if (b >= 0x20 && b < 0x7f)
      return key_event{key_code::character, static_cast<char>(b)};
    return {};
  }

  static key_event final_byte(std::uint8_t b) noexcept {
    switch (b) {
    case 'A':
      return make(key_code::up);
    case 'B':
      return make(key_code::down);
    case 'C':
      return make(key_code::right);
    case 'D':
      return make(key_code::left);
    case 'H':
      return make(key_code::home);
    case 'F':
      return make(key_code::end);
    default:
      return {};
    }
  }

  static key_event tilde(int param) noexcept {
    switch (param) {
    case 1:
    case 7:
      return make(key_code::home);
    case 3:
      return make(key_code::del);
    case 4:
    case 8:
      return make(key_code::end);
    case 5:
      return make(key_code::page_up);
    case 6:
      return make(key_code::page_down);
    default:
      return {};
    }
  }

  state state_ = state::ground;
  bool after_cr_ = false;
  int param_ = 0;
};

/** @brief What `text_editor::handle` asks the caller to do next. */
enum class edit_action : std::uint8_t { none, save, cancel };

/**
 * @brief Editing state over a caller-owned `reloco::string`. The string must outlive the editor and
 * must not be changed behind its back while editing. Inserting can fail with the allocator's error
 * (`allocation_failed`); the text is then unchanged.
 */
class text_editor {
public:
  /** @param tab_width spaces per tab stop (>= 1). The cursor starts at the beginning of the text. */
  explicit text_editor(reloco::string &buf, std::size_t tab_width = 2) noexcept
      : buf_(&buf), tab_width_(tab_width == 0 ? 1 : tab_width) {}

  // ---- state ------------------------------------------------------------------------------------

  [[nodiscard]] std::size_t cursor() const noexcept { return pos_; }
  /** @brief Zero-based line / column of the cursor. */
  [[nodiscard]] std::size_t line() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < pos_; ++i)
      if ((*buf_)[i] == '\n')
        ++n;
    return n;
  }
  [[nodiscard]] std::size_t column() const noexcept { return pos_ - line_start(pos_); }
  [[nodiscard]] std::size_t line_count() const noexcept {
    std::size_t n = 1;
    for (std::size_t i = 0; i < buf_->size(); ++i)
      if ((*buf_)[i] == '\n')
        ++n;
    return n;
  }
  [[nodiscard]] bool modified() const noexcept { return modified_; }
  [[nodiscard]] reloco::string_view text() const noexcept { return buf_->view(); }

  /** @brief Sets the screen size used for scrolling and PageUp/PageDown; the last row is the status bar. */
  void set_screen(std::size_t rows, std::size_t cols) noexcept {
    rows_ = rows < 2 ? 1 : rows - 1;
    cols_ = cols == 0 ? 1 : cols;
  }
  /** @brief Text shown in the status bar (not copied; pass a literal or an empty view). */
  void set_message(reloco::string_view msg) noexcept { message_ = msg; }

  // ---- editing ----------------------------------------------------------------------------------

  [[nodiscard]] reloco::result<void> insert(char c) noexcept {
    if (auto r = buf_->try_insert(pos_, reloco::string_view(&c, 1)); !r)
      return r;
    ++pos_;
    modified_ = true;
    want_col_ = column();
    return {};
  }

  [[nodiscard]] reloco::result<void> insert_text(reloco::string_view s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i)
      if (auto r = insert(s[i]); !r)
        return r;
    return {};
  }

  /** @brief Inserts spaces up to the next tab stop. */
  [[nodiscard]] reloco::result<void> insert_tab() noexcept {
    std::size_t n = tab_width_ - column() % tab_width_;
    while (n-- > 0)
      if (auto r = insert(' '); !r)
        return r;
    return {};
  }

  void backspace() noexcept {
    if (pos_ == 0)
      return;
    buf_->erase(pos_ - 1, 1);
    --pos_;
    modified_ = true;
    want_col_ = column();
  }

  void del() noexcept {
    if (pos_ >= buf_->size())
      return;
    buf_->erase(pos_, 1);
    modified_ = true;
  }

  /** @brief Erases to the end of the line; on an empty remainder it joins the next line. */
  void kill_line() noexcept {
    const std::size_t le = line_end(pos_);
    if (le == pos_)
      del();
    else {
      buf_->erase(pos_, le - pos_);
      modified_ = true;
    }
  }

  // ---- movement ---------------------------------------------------------------------------------

  void left() noexcept {
    if (pos_ > 0)
      --pos_;
    want_col_ = column();
  }
  void right() noexcept {
    if (pos_ < buf_->size())
      ++pos_;
    want_col_ = column();
  }
  void home() noexcept {
    pos_ = line_start(pos_);
    want_col_ = 0;
  }
  void end() noexcept {
    pos_ = line_end(pos_);
    want_col_ = column();
  }
  void up() noexcept {
    const std::size_t ls = line_start(pos_);
    if (ls == 0)
      return;
    const std::size_t prev_end = ls - 1;
    const std::size_t prev_start = line_start(prev_end);
    pos_ = prev_start + min(want_col_, prev_end - prev_start);
  }
  void down() noexcept {
    const std::size_t le = line_end(pos_);
    if (le >= buf_->size())
      return;
    const std::size_t next_start = le + 1;
    pos_ = next_start + min(want_col_, line_end(next_start) - next_start);
  }
  void page_up() noexcept {
    for (std::size_t i = 0; i < rows_; ++i)
      up();
  }
  void page_down() noexcept {
    for (std::size_t i = 0; i < rows_; ++i)
      down();
  }

  /**
   * @brief Applies one key. Returns `save`/`cancel` when the user asked to leave, otherwise `none`;
   * an insert failure is returned as the error (the key is ignored).
   */
  [[nodiscard]] reloco::result<edit_action> handle(const key_event &ev) noexcept {
    switch (ev.code) {
    case key_code::character:
      if (auto r = insert(ev.ch); !r)
        return reloco::unexpected(r.error());
      break;
    case key_code::enter:
      if (auto r = insert('\n'); !r)
        return reloco::unexpected(r.error());
      break;
    case key_code::tab:
      if (auto r = insert_tab(); !r)
        return reloco::unexpected(r.error());
      break;
    case key_code::backspace:
      backspace();
      break;
    case key_code::del:
      del();
      break;
    case key_code::left:
      left();
      break;
    case key_code::right:
      right();
      break;
    case key_code::up:
      up();
      break;
    case key_code::down:
      down();
      break;
    case key_code::home:
      home();
      break;
    case key_code::end:
      end();
      break;
    case key_code::page_up:
      page_up();
      break;
    case key_code::page_down:
      page_down();
      break;
    case key_code::kill_line:
      kill_line();
      break;
    case key_code::save:
      return edit_action::save;
    case key_code::cancel:
      return edit_action::cancel;
    case key_code::redraw:
    case key_code::none:
      break;
    }
    return edit_action::none;
  }

  // ---- display ----------------------------------------------------------------------------------

  /**
   * @brief Scrolls so the cursor is visible, then redraws the whole screen: the text rows, an
   * inverse-video status bar on the last row, and the cursor. Uses ANSI sequences only.
   */
  [[nodiscard]] reloco::result<void> render(const hw::uart_ref &uart) noexcept {
    scroll_to_cursor();
    if (auto r = uart.write_string("\x1b[?25l"); !r) // hide the cursor while drawing
      return r;

    std::size_t offset = line_offset(top_line_);
    bool have_line = true;
    for (std::size_t row = 0; row < rows_; ++row) {
      if (auto r = move_to(uart, row + 1, 1); !r)
        return r;
      if (have_line) {
        const std::size_t le = line_end(offset);
        if (auto r = write_clipped(uart, offset, le); !r)
          return r;
        if (le >= buf_->size())
          have_line = false;
        else
          offset = le + 1;
      } else if (auto r = uart.write_string("~"); !r) {
        return r;
      }
      if (auto r = uart.write_string("\x1b[K"); !r)
        return r;
    }

    if (auto r = move_to(uart, rows_ + 1, 1); !r)
      return r;
    const auto status = microfmt::format<96>("\x1b[7m Ln {}, Col {} {}  ^S save  ^X quit  {}", line() + 1, column() + 1,
                                             modified_ ? "[modified]" : "", message_);
    if (auto r = uart.write_string(status.view()); !r)
      return r;
    if (auto r = uart.write_string("\x1b[K\x1b[0m"); !r)
      return r;

    if (auto r = move_to(uart, line() - top_line_ + 1, column() - left_col_ + 1); !r)
      return r;
    return uart.write_string("\x1b[?25h");
  }

private:
  static std::size_t min(std::size_t a, std::size_t b) noexcept { return a < b ? a : b; }

  [[nodiscard]] std::size_t line_start(std::size_t pos) const noexcept {
    while (pos > 0 && (*buf_)[pos - 1] != '\n')
      --pos;
    return pos;
  }
  [[nodiscard]] std::size_t line_end(std::size_t pos) const noexcept {
    while (pos < buf_->size() && (*buf_)[pos] != '\n')
      ++pos;
    return pos;
  }
  [[nodiscard]] std::size_t line_offset(std::size_t line_no) const noexcept {
    std::size_t pos = 0;
    while (line_no > 0 && pos < buf_->size()) {
      pos = line_end(pos);
      if (pos < buf_->size())
        ++pos;
      --line_no;
    }
    return pos;
  }

  void scroll_to_cursor() noexcept {
    const std::size_t l = line();
    const std::size_t c = column();
    if (l < top_line_)
      top_line_ = l;
    else if (l >= top_line_ + rows_)
      top_line_ = l - rows_ + 1;
    if (c < left_col_)
      left_col_ = c;
    else if (c >= left_col_ + cols_)
      left_col_ = c - cols_ + 1;
  }

  static reloco::result<void> move_to(const hw::uart_ref &uart, std::size_t row, std::size_t col) noexcept {
    const auto seq = microfmt::format<24>("\x1b[{};{}H", row, col);
    return uart.write_string(seq.view());
  }

  // Writes the visible columns of [begin, end) with control characters shown as '?'.
  reloco::result<void> write_clipped(const hw::uart_ref &uart, std::size_t begin, std::size_t end) const noexcept {
    for (std::size_t i = begin + left_col_; i < end && i < begin + left_col_ + cols_; ++i) {
      const auto c = static_cast<std::uint8_t>((*buf_)[i]);
      if (auto r = uart.put_byte(c < 0x20 || c == 0x7f ? std::uint8_t{'?'} : c); !r)
        return r;
    }
    return {};
  }

  reloco::string *buf_;
  std::size_t tab_width_;
  std::size_t pos_ = 0;
  std::size_t want_col_ = 0; // column kept while moving through shorter lines
  std::size_t top_line_ = 0;
  std::size_t left_col_ = 0;
  std::size_t rows_ = 23; // text rows (screen rows minus the status bar)
  std::size_t cols_ = 80;
  bool modified_ = false;
  reloco::string_view message_;
};

} // namespace structo::bootldr

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

/** @brief Screen geometry and tab width for `edit_text`. */
struct editor_options {
  std::size_t rows = 24;
  std::size_t cols = 80;
  std::size_t tab_width = 2;
};

/**
 * @brief Runs the editor on `uart` until the user saves or cancels. Resolves to `true` after Ctrl-S
 * (the string holds the edited text) and `false` after a confirmed Ctrl-X (the string is restored to
 * its original text). Fails with the UART's or the allocator's error. Other scheduler tasks keep
 * running between keys. The caller must not use the UART or touch `buf` meanwhile.
 */
[[nodiscard]] inline reloco::task<bool> edit_text(scheduler &sched, hw::uart_ref uart, reloco::string &buf,
                                                  const editor_options &opt = {}) noexcept {
  reloco::string original(buf.get_allocator()); // to restore on cancel
  if (auto r = original.try_assign(buf.view()); !r)
    co_await reloco::unexpected(r.error());

  text_editor ed(buf, opt.tab_width);
  ed.set_screen(opt.rows, opt.cols);
  key_decoder dec;
  bool dirty = true;
  bool confirm_discard = false;
  (void)uart.write_string("\x1b[0m\x1b[2J");

  bool saved = false;
  for (;;) {
    auto ready = uart.rx_ready();
    if (!ready)
      co_await reloco::unexpected(ready.error());
    if (!*ready) {
      if (dirty) { // redraw once the input burst is processed
        if (auto r = ed.render(uart); !r)
          co_await reloco::unexpected(r.error());
        dirty = false;
      }
      co_await sched.yield();
      continue;
    }

    auto got = uart.try_get_byte();
    if (!got) {
      if (got.error() == reloco::error::try_again)
        continue;
      co_await reloco::unexpected(got.error());
    }
    const key_event ev = dec.feed(*got);
    if (ev.code == key_code::none)
      continue;
    dirty = true;

    if (ev.code != key_code::cancel && confirm_discard) {
      confirm_discard = false;
      ed.set_message({});
    }
    auto act = ed.handle(ev);
    if (!act) {
      ed.set_message("out of memory"); // the key was ignored
      (void)uart.put_byte(0x07);
      continue;
    }
    if (*act == edit_action::save) {
      saved = true;
      break;
    }
    if (*act == edit_action::cancel) {
      if (ed.modified() && !confirm_discard) {
        confirm_discard = true;
        ed.set_message("unsaved changes: ^X again to discard");
        continue;
      }
      break;
    }
    if (ev.code == key_code::redraw)
      (void)uart.write_string("\x1b[2J");
  }

  if (!saved && ed.modified()) {
    if (auto r = buf.try_assign(original.view()); !r)
      co_await reloco::unexpected(r.error());
  }
  (void)uart.write_string("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
  co_return saved;
}

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
