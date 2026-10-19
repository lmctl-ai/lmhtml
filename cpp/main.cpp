#include "dslvm.hpp"
#include "analyze.hpp"
#include "html.hpp"
#include "schema_embedded.hpp"
#include "strlib.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <vector>

namespace {
using dslvm::Args;
using dslvm::Value;

struct Report {
  struct Finding {
    std::string kind, file, message;
    int64_t line;
  };
  std::vector<Finding> findings;
};

struct Host {
  std::vector<std::unique_ptr<lmhtml::Document>> docs;
  std::vector<std::unique_ptr<std::vector<lxb_dom_node_t *>>> node_lists;
  std::vector<std::unique_ptr<std::vector<std::string>>> str_lists;
  std::vector<std::unique_ptr<std::vector<lmhtml::Document *>>> doc_lists;
  Report report;
  bool dry_run = false; // --dry-run: Html.save reports would-save, writes nothing
  std::map<const lmhtml::Document *, std::string> dir_roots; // Html.load_dir pages
  std::string root_of(const lmhtml::Document *doc) const {
    auto it = dir_roots.find(doc);
    return it == dir_roots.end() ? "." : it->second;
  }
  template <class T>
  Value hold(std::vector<std::unique_ptr<T>> &arena, const char *type, T value) {
    if (arena.size() >= 65536)
      throw dslvm::Error(std::string(type) + " object limit exceeded (65536)");
    arena.push_back(std::make_unique<T>(std::move(value)));
    return Value::object(type, arena.back().get());
  }
};

template <class T> T *self(void *p) { return static_cast<T *>(p); }

lmhtml::Document *doc_of(void *p, const char *who) {
  auto *doc = self<lmhtml::Document>(p);
  if (!doc || !doc->tree) throw dslvm::Error(std::string(who) + ": stale document handle");
  return doc;
}
lxb_dom_node_t *node_of(void *p, const char *who) {
  auto *node = self<lxb_dom_node_t>(p);
  if (!node) throw dslvm::Error(std::string(who) + ": null node handle");
  return node;
}

// Largest HTML file the host reads (guards against huge or endless inputs).
constexpr std::uintmax_t kMaxHtmlBytes = 256u << 20;

std::string read_file(const std::string &path) {
  std::error_code ec;
  // Directories, FIFOs and devices (/dev/zero) would read empty or forever.
  if (!std::filesystem::is_regular_file(path, ec)) throw dslvm::Error("cannot read " + path);
  if (std::filesystem::file_size(path, ec) > kMaxHtmlBytes && !ec)
    throw dslvm::Error("cannot read " + path + ": larger than 256 MiB");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw dslvm::Error("cannot read " + path);
  std::ostringstream data;
  data << in.rdbuf();
  if (in.bad()) throw dslvm::Error("cannot read " + path);
  return data.str();
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 when invalid
// (overlong forms, surrogates and code points above U+10FFFF included).
size_t utf8_sequence(const std::string &s, size_t i) {
  auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
  unsigned char c = byte(i);
  size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
  if (n == 0 || i + n > s.size()) return 0;
  uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
  for (size_t k = 1; k < n; k++) {
    if ((byte(i + k) & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  static const uint32_t min_for_len[] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < min_for_len[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return n;
}

// JSON string body. HTML input is arbitrary bytes (lexbor keeps them as-is),
// so invalid UTF-8 becomes U+FFFD to keep the report valid JSON.
std::string json_escape(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = utf8_sequence(s, i);
    if (n == 0) {
      out += "\\ufffd";
      i++;
      continue;
    }
    if (n > 1) {
      out.append(s, i, n);
      i += n;
      continue;
    }
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20 || c == 0x7f) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
    i++;
  }
  return out;
}

// Text report fields: one finding per line, so control characters (a newline
// inside a URL would forge a second finding) are shown escaped.
std::string text_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

void write_report_text(const Report &report, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw dslvm::Error("cannot write " + path);
  for (const auto &f : report.findings)
    out << text_escape(f.kind) << " " << text_escape(f.file) << ":" << f.line << ": "
        << text_escape(f.message) << "\n";
  if (!out) throw dslvm::Error("failed writing " + path);
}

void write_report_json(const Report &report, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw dslvm::Error("cannot write " + path);
  out << "{\"findings\":[";
  for (size_t i = 0; i < report.findings.size(); i++) {
    const auto &f = report.findings[i];
    if (i) out << ",";
    out << "{\"kind\":\"" << json_escape(f.kind) << "\",\"file\":\"" << json_escape(f.file)
        << "\",\"line\":" << f.line << ",\"message\":\"" << json_escape(f.message) << "\"}";
  }
  out << "],\"count\":" << report.findings.size() << "}\n";
  if (!out) throw dslvm::Error("failed writing " + path);
}

// Lexical normalization: "a/./b/../c" -> "a/c"; leading ".." kept.
std::string normalize_path(const std::string &path) { return lmhtml::resolve_relative("", path); }

std::string percent_decode(const std::string &s) {
  auto hex = [](char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  };
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool is_external(const std::string &url) {
  return !lmhtml::url_scheme(url).empty() || url.rfind("//", 0) == 0;
}

// Where a link points on disk. Policy (v0):
// - leading/trailing ASCII whitespace is stripped and tab/newline removed
//   (HTML URL parsing); http(s), mailto:, javascript:, data:, protocol-
//   relative "//host" and anything under an absolute <base href> are
//   external: unchecked, they count as existing;
// - ?query is ignored; path and #fragment are percent-decoded;
// - relative paths resolve against <base href> (if relative) or the page;
//   root-relative "/x" resolves against the site root: the Html.load_dir
//   directory, or the current directory for Html.load / Html.parse;
// - pages from Html.load_dir must not escape their directory ("../" above
//   the root is broken); a directory link means its index.html.
struct LinkTarget {
  bool external = false, outside_root = false;
  std::string file;      // normalized path; the page itself for "#x" / "?q"
  std::string fragment;  // raw (still percent-encoded), without '#'
  bool has_fragment = false;
  bool same_page = false; // "#x" / "?q" without <base href>: the page itself
};

// What a browser does to an href before parsing it (WHATWG URL): strip
// leading/trailing C0 controls and spaces, drop every tab/newline (so
// "java\tscript:" is javascript:), and read '\' as '/' unless the URL has a
// non-web scheme (so "\\evil.com" is the protocol-relative //evil.com).
std::string clean_url(const std::string &raw) {
  auto c0_or_space = [](char c) { return static_cast<unsigned char>(c) <= 0x20; };
  size_t b = 0, e = raw.size();
  while (b < e && c0_or_space(raw[b])) b++;
  while (e > b && c0_or_space(raw[e - 1])) e--;
  std::string url;
  for (size_t i = b; i < e; i++)
    if (raw[i] != '\t' && raw[i] != '\n' && raw[i] != '\r') url += raw[i];
  std::string scheme = lmhtml::strlib::lower(lmhtml::url_scheme(url));
  if (scheme.empty() || scheme == "http" || scheme == "https" || scheme == "file" ||
      scheme == "ws" || scheme == "wss" || scheme == "ftp")
    std::replace(url.begin(), url.end(), '\\', '/');
  return url;
}

LinkTarget resolve_link(Host &h, lmhtml::Document *doc, const std::string &raw) {
  LinkTarget t;
  std::string url = clean_url(raw);
  if (is_external(url)) {
    t.external = true;
    return t;
  }
  std::string::size_type hash = url.find('#');
  if (hash != std::string::npos) {
    t.has_fragment = true;
    t.fragment = url.substr(hash + 1);
    url.erase(hash);
  }
  url.erase(std::min(url.size(), url.find('?')));
  std::string base_file = doc->path;
  auto bases = lmhtml::select(lxb_dom_interface_node(doc->tree), "base[href]");
  if (!bases.empty()) {
    std::string base = lmhtml::attr(bases[0], "href");
    if (is_external(base)) {
      t.external = true;
      return t;
    }
    base = percent_decode(base.substr(0, std::min(base.find_first_of("?#"), base.size())));
    if (!base.empty()) {
      base_file = base[0] == '/' ? h.root_of(doc) + base : lmhtml::resolve_relative(doc->path, base);
      if (base.back() == '/') base_file += "/index.html";
    }
  }
  std::string path = percent_decode(url);
  std::string root = h.root_of(doc);
  if (path.empty() && bases.empty()) {
    t.same_page = true;
    t.file = doc->path;
    return t;
  }
  if (path.empty()) t.file = base_file;
  else if (path[0] == '/') t.file = root + path;
  else t.file = lmhtml::resolve_relative(base_file, path);
  t.file = normalize_path(t.file);
  if (t.file.empty()) t.file = ".";
  if (h.dir_roots.count(doc)) {
    std::string r = normalize_path(root);
    t.outside_root = r.empty() || r == "." ? t.file == ".." || t.file.rfind("../", 0) == 0 || t.file[0] == '/'
                                           : t.file != r && t.file.rfind(r + "/", 0) != 0;
  }
  std::error_code ec;
  if (std::filesystem::is_directory(t.file, ec)) t.file += "/index.html";
  return t;
}

lmhtml::Document *find_loaded(Host &h, const std::string &file) {
  for (auto &d : h.docs)
    if (normalize_path(d->path) == file) return d.get();
  return nullptr;
}

bool link_exists(Host &h, lmhtml::Document *doc, const std::string &url) {
  LinkTarget t = resolve_link(h, doc, url);
  if (t.external) return true;
  // "#" and "#top" always scroll to the top (HTML spec); other fragments
  // must match an id or a[name], raw or percent-decoded.
  auto anchor_ok = [&](lmhtml::Document &target) {
    return !t.has_fragment || t.fragment.empty() || t.fragment == "top" ||
           lmhtml::has_anchor(target, t.fragment) ||
           lmhtml::has_anchor(target, percent_decode(t.fragment));
  };
  if (t.same_page) return anchor_ok(*doc);
  if (t.outside_root || t.file.find('\0') != std::string::npos) return false;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(t.file, ec)) return false;
  if (!t.has_fragment || t.fragment.empty() || t.fragment == "top") return true;
  lmhtml::Document *target = normalize_path(doc->path) == t.file ? doc : find_loaded(h, t.file);
  std::unique_ptr<lmhtml::Document> loaded;
  if (!target) {
    loaded = lmhtml::parse_html(read_file(t.file), t.file);
    target = loaded.get();
  }
  return anchor_ok(*target);
}

// Link classification so scripts can ignore whole categories instead of
// post-filtering: external (http(s), //host, absolute <base href>),
// special (mailto/tel/javascript/data/...), anchor (same-page #fragment),
// outside-root (escapes the load_dir root), internal-page (.html/.htm or a
// directory), internal-asset (anything else). Existence stays link_exists.
std::string link_kind(Host &h, lmhtml::Document *doc, const std::string &raw) {
  std::string url = clean_url(raw);
  std::string scheme = lmhtml::strlib::lower(lmhtml::url_scheme(url));
  if (!scheme.empty()) return scheme == "http" || scheme == "https" ? "external" : "special";
  // Same rules as link_exists: <base href> decides what "#x" points at.
  LinkTarget t = resolve_link(h, doc, url);
  if (t.external) return "external";
  if (t.same_page) return t.has_fragment ? "anchor" : "internal-page";
  if (t.outside_root) return "outside-root";
  std::string file = lmhtml::strlib::lower(t.file);
  std::string::size_type dot = file.rfind('.');
  std::string ext = dot == std::string::npos ? "" : file.substr(dot);
  if (ext == ".html" || ext == ".htm") return "internal-page";
  std::error_code ec;
  if (std::filesystem::is_directory(t.file, ec)) return "internal-page";
  return "internal-asset";
}

// Host callbacks must surface every failure as a DSL error: lexbor wrappers
// throw std::string, std::filesystem and allocation failures throw
// std::exception; anything escaping main would abort the process.
template <class F> Value translate_errors(F &&fn) {
  try {
    return fn();
  } catch (const dslvm::Error &) {
    throw;
  } catch (const std::string &e) {
    throw dslvm::Error(e);
  } catch (const std::exception &e) {
    throw dslvm::Error(e.what());
  }
}

// Write `content` to `target` (canonical, verified to lie under `root`).
// POSIX: without following symlinks anywhere below the root (O_NOFOLLOW dir
// walk), data goes to a fresh O_EXCL temp file, is fsync'ed, takes over the
// replaced file's mode, and is renamed into place. Windows (MinGW has no
// openat/O_NOFOLLOW/fchmod): reduced guarantee — the directory walk and mode
// preservation are skipped; atomicity is kept via temp file + MoveFileEx
// replace. The path itself is still verified against the root beforehand.
#ifdef _WIN32
void write_inside_root(const std::string &root, const std::string &target,
                       const std::string &content, const std::string &shown) {
  auto fail = [&](const std::string &why) { throw dslvm::Error("Html.save: " + why + ": " + shown); };
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(target).parent_path(), ec);
  if (ec) fail("cannot create directories");
  static unsigned counter = 0;
  std::string tmp = target + ".lmhtml-" + std::to_string(_getpid()) + "-" +
                    std::to_string(++counter);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) fail("cannot create temp file");
    out << content;
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(tmp, ec);
      fail("failed writing");
    }
  }
  if (!MoveFileExA(tmp.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(tmp, ec);
    fail("cannot rename into place");
  }
  (void) root;
}
#else
void write_inside_root(const std::string &root, const std::string &target,
                       const std::string &content, const std::string &shown) {
  auto fail = [&](const std::string &why) {
    throw dslvm::Error("Html.save: " + why + ": " + shown + " (" + std::strerror(errno) + ")");
  };
  int dir = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) fail("cannot open output root");
  struct Fd {
    int &fd;
    ~Fd() { if (fd >= 0) ::close(fd); }
  } dir_guard{dir};
  std::filesystem::path rel = std::filesystem::path(target).lexically_relative(root);
  std::vector<std::string> parts;
  for (const auto &part : rel) parts.push_back(part.string());
  if (parts.empty()) fail("empty path");
  const std::string name = parts.back();
  if (name == "." || name == "..") fail("path must name a file");
  for (size_t i = 0; i + 1 < parts.size(); i++) {
    if (parts[i] == "." || parts[i] == "..") fail("unexpected path component");
    if (::mkdirat(dir, parts[i].c_str(), 0777) < 0 && errno != EEXIST) fail("cannot create directory");
    int next = ::openat(dir, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (next < 0) fail("cannot enter directory (symlink or not a directory)");
    ::close(dir);
    dir = next;
  }
  struct stat existing {};
  mode_t mode;
  if (::fstatat(dir, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode)) {
      errno = EISDIR;
      fail("target is not a regular file");
    }
    mode = existing.st_mode & 07777;
  } else {
    mode_t mask = ::umask(0);
    ::umask(mask);
    mode = 0666 & ~mask;
  }
  static unsigned counter = 0;
  std::string tmp;
  int fd = -1;
  for (int attempt = 0; fd < 0 && attempt < 100; attempt++) {
    tmp = "." + name + ".lmhtml-" + std::to_string(::getpid()) + "-" + std::to_string(++counter) +
          "-" + std::to_string(std::random_device{}() & 0xffffff);
    fd = ::openat(dir, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 && errno != EEXIST) break;
  }
  if (fd < 0) fail("cannot create temp file");
  auto discard = [&](const std::string &why) {
    int saved = errno;
    ::close(fd);
    ::unlinkat(dir, tmp.c_str(), 0);
    errno = saved;
    fail(why);
  };
  for (size_t off = 0; off < content.size();) {
    ssize_t n = ::write(fd, content.data() + off, content.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) discard("failed writing");
    off += static_cast<size_t>(n);
  }
  if (::fchmod(fd, mode) < 0) discard("cannot set file mode");
  if (::fsync(fd) < 0) discard("failed writing");
  if (::close(fd) < 0) {
    fd = -1;
    int saved = errno;
    ::unlinkat(dir, tmp.c_str(), 0);
    errno = saved;
    fail("failed writing");
  }
  if (::renameat(dir, tmp.c_str(), dir, name.c_str()) < 0) {
    int saved = errno;
    ::unlinkat(dir, tmp.c_str(), 0);
    errno = saved;
    fail("cannot rename into place");
  }
}
#endif

// load_dir include/exclude patterns match the path relative to the
// directory ("blog/2024/a.html"). "./x" means "x"; an absolute pattern is
// accepted only below the directory (made relative); one that leaves it
// ("..", other absolute paths) could never match and is refused.
std::string dir_pattern(const std::string &dir, const std::string &pattern, const char *which) {
  std::string pat = pattern;
  while (pat.rfind("./", 0) == 0) pat.erase(0, 2);
  // absolute: POSIX /x or Windows drive-letter C:/x
  bool absoluteish = !pat.empty() && pat[0] == '/';
  if (pat.size() > 2 && std::isalpha(static_cast<unsigned char>(pat[0])) && pat[1] == ':' &&
      pat[2] == '/')
    absoluteish = true;
  if (absoluteish) {
    std::error_code ec;
    std::string base = std::filesystem::absolute(dir, ec).lexically_normal().generic_string();
    if (!base.empty() && base.back() != '/') base += '/';
    if (ec || pat.rfind(base, 0) != 0)
      throw dslvm::Error(std::string("Html.load_dir: ") + which + " pattern is outside " + dir +
                         ": " + pattern);
    pat.erase(0, base.size());
  }
  if (pat == ".." || pat.rfind("../", 0) == 0 || pat.find("/../") != std::string::npos)
    throw dslvm::Error(std::string("Html.load_dir: ") + which + " pattern leaves " + dir + ": " +
                       pattern);
  return pat;
}

// Glob on a relative path; `*` crosses '/', and "**/" also matches zero
// directories (gitignore): "**/a.html" matches "a.html", "x/**/a.html"
// matches "x/a.html".
bool glob_path_match(const std::string &rel, const std::string &pat) {
  if (lmhtml::strlib::match(rel, pat)) return true;
  std::string collapsed = pat;
  while (collapsed.rfind("**/", 0) == 0) collapsed.erase(0, 3);
  for (size_t at; (at = collapsed.find("/**/")) != std::string::npos;) collapsed.replace(at, 4, "/");
  return collapsed != pat && lmhtml::strlib::match(rel, collapsed);
}

void checked_method(dslvm::Registry &registry, const char *type, const char *name,
                    std::vector<std::string> params, std::string returns,
                    dslvm::Method callback) {
  registry.method(type, name, params, returns,
                  [params, callback, type, name](void *object, Args &args) {
                    for (size_t i = 0; i < params.size(); i++) {
                      const auto &expected = params[i];
                      const auto &value = args[i];
                      bool valid = expected == "float"   ? value.kind == Value::FLOAT
                                   : expected == "int"   ? value.kind == Value::INT
                                   : expected == "string" ? value.kind == Value::STR
                                   : expected == "bool"   ? value.kind == Value::BOOL
                                   : value.kind == Value::OBJ && value.type == expected &&
                                         value.obj;
                      if (!valid)
                        throw dslvm::Error(std::string(type) + "." + name + " argument " +
                                           std::to_string(i + 1) + " must be " + expected);
                    }
                    return translate_errors([&] { return callback(object, args); });
                  });
}

void checked_function(dslvm::Registry &r, const char *name, std::vector<std::string> params,
                      std::string returns, dslvm::Function fn) {
  r.function(name, params, returns, [params, fn, name](Args &args) {
    if (args.size() != params.size())
      throw dslvm::Error(std::string(name) + " argument count mismatch");
    for (size_t i = 0; i < params.size(); ++i) {
      const auto &v = args[i];
      const auto &e = params[i];
      bool ok = e == "float"   ? v.kind == Value::FLOAT
                : e == "int"   ? v.kind == Value::INT
                : e == "string" ? v.kind == Value::STR
                : e == "bool"   ? v.kind == Value::BOOL
                                : v.kind == Value::OBJ && v.type == e && v.obj;
      if (!ok)
        throw dslvm::Error(std::string(name) + " argument " + std::to_string(i + 1) +
                           " must be " + e);
    }
    return translate_errors([&] { return fn(args); });
  });
}

template <class L> Value wrap_lex(const char *who, L &&fn) {
  try {
    return fn();
  } catch (const dslvm::Error &) {
    throw;
  } catch (const std::string &e) {
    throw dslvm::Error(std::string(who) + ": " + e);
  }
}

void register_host(dslvm::Registry &r, Host &h) {
  r.context("html", "Html");
  r.context("report", "Report");

  checked_method(r, "Doc", "select", {"string"}, "NodeList", [&h](void *p, Args &a) {
    auto *doc = doc_of(p, "Doc.select");
    return wrap_lex("Doc.select", [&] {
      auto nodes = lmhtml::select(lxb_dom_interface_node(doc->tree), a[0].s);
      return h.hold(h.node_lists, "NodeList", std::move(nodes));
    });
  });
  checked_method(r, "Doc", "path", {}, "string",
                 [](void *p, Args &) { return Value::string(self<lmhtml::Document>(p)->path); });
  checked_method(r, "Doc", "links", {}, "StrList", [&h](void *p, Args &) {
    auto *doc = doc_of(p, "Doc.links");
    return h.hold(h.str_lists, "StrList", lmhtml::collect_links(*doc));
  });
  checked_method(r, "Doc", "classes_used", {}, "StrList", [&h](void *p, Args &) {
    return h.hold(h.str_lists, "StrList", lmhtml::classes_used(*doc_of(p, "Doc.classes_used")));
  });
  // External stylesheets resolve like links: external/missing files skip.
  auto css_resolver = [&h](lmhtml::Document *doc) {
    return [&h, doc](const std::string &href) -> std::string {
      LinkTarget t = resolve_link(h, doc, href);
      if (t.external || t.same_page || t.file.empty()) return "";
      std::error_code ec;
      return std::filesystem::is_regular_file(t.file, ec) ? t.file : "";
    };
  };
  checked_method(r, "Doc", "css_classes", {}, "StrList", [&h, css_resolver](void *p, Args &) {
    auto *doc = doc_of(p, "Doc.css_classes");
    std::vector<std::string> classes, ids;
    lmhtml::css_names(*doc, classes, ids, css_resolver(doc));
    return h.hold(h.str_lists, "StrList", std::move(classes));
  });
  checked_method(r, "Doc", "css_ids", {}, "StrList", [&h, css_resolver](void *p, Args &) {
    auto *doc = doc_of(p, "Doc.css_ids");
    std::vector<std::string> classes, ids;
    lmhtml::css_names(*doc, classes, ids, css_resolver(doc));
    return h.hold(h.str_lists, "StrList", std::move(ids));
  });
  checked_method(r, "Doc", "ids", {}, "StrList", [&h](void *p, Args &) {
    return h.hold(h.str_lists, "StrList", lmhtml::ids_used(*doc_of(p, "Doc.ids")));
  });

  checked_method(r, "Node", "tag", {}, "string", [](void *p, Args &) {
    return Value::string(lmhtml::tag_name(node_of(p, "Node.tag")));
  });
  checked_method(r, "Node", "attr", {"string"}, "string", [](void *p, Args &a) {
    return Value::string(lmhtml::attr(node_of(p, "Node.attr"), a[0].s));
  });
  checked_method(r, "Node", "has_attr", {"string"}, "bool", [](void *p, Args &a) {
    return Value::boolean(lmhtml::has_attr(node_of(p, "Node.has_attr"), a[0].s));
  });
  checked_method(r, "Node", "set_attr", {"string", "string"}, "Node", [](void *p, Args &a) {
    auto *node = node_of(p, "Node.set_attr");
    try {
      lmhtml::set_attr(node, a[0].s, a[1].s);
    } catch (const std::string &e) {
      throw dslvm::Error("Node.set_attr: " + e);
    }
    return Value::object("Node", node);
  });
  checked_method(r, "Node", "text", {}, "string", [](void *p, Args &) {
    return Value::string(lmhtml::text_content(node_of(p, "Node.text")));
  });
  checked_method(r, "Node", "outer_html", {}, "string", [](void *p, Args &) {
    return wrap_lex("Node.outer_html",
                    [&] { return Value::string(lmhtml::outer_html(node_of(p, "Node.outer_html"))); });
  });
  checked_method(r, "Node", "parent", {}, "Node", [](void *p, Args &) {
    lxb_dom_node_t *parent = lmhtml::parent_node(node_of(p, "Node.parent"));
    if (!parent) throw dslvm::Error("Node.parent: node has no parent element");
    return Value::object("Node", parent);
  });
  checked_method(r, "Node", "select", {"string"}, "NodeList", [&h](void *p, Args &a) {
    auto *node = node_of(p, "Node.select");
    return wrap_lex("Node.select", [&] {
      return h.hold(h.node_lists, "NodeList", lmhtml::select(node, a[0].s));
    });
  });
  checked_method(r, "Node", "remove", {}, "void", [](void *p, Args &) {
    try {
      lmhtml::remove_node(node_of(p, "Node.remove"));
    } catch (const std::string &e) {
      throw dslvm::Error("Node.remove: " + e);
    }
    return Value::none();
  });
  checked_method(r, "Node", "replace_with", {"string"}, "Node", [](void *p, Args &a) {
    try {
      return Value::object("Node",
                           lmhtml::replace_with_html(node_of(p, "Node.replace_with"), a[0].s));
    } catch (const std::string &e) {
      throw dslvm::Error("Node.replace_with: " + e);
    }
  });
  checked_method(r, "Node", "insert_before", {"string"}, "Node", [](void *p, Args &a) {
    try {
      return Value::object("Node",
                           lmhtml::insert_html(node_of(p, "Node.insert_before"), a[0].s, true));
    } catch (const std::string &e) {
      throw dslvm::Error("Node.insert_before: " + e);
    }
  });
  checked_method(r, "Node", "insert_after", {"string"}, "Node", [](void *p, Args &a) {
    try {
      return Value::object("Node",
                           lmhtml::insert_html(node_of(p, "Node.insert_after"), a[0].s, false));
    } catch (const std::string &e) {
      throw dslvm::Error("Node.insert_after: " + e);
    }
  });
  checked_method(r, "Node", "hash", {}, "int", [](void *p, Args &) {
    return Value::integer(static_cast<int64_t>(lmhtml::subtree_hash(node_of(p, "Node.hash"))));
  });

  checked_method(r, "NodeList", "len", {}, "int", [](void *p, Args &) {
    return Value::integer(static_cast<int64_t>(self<std::vector<lxb_dom_node_t *>>(p)->size()));
  });
  checked_method(r, "NodeList", "at", {"int"}, "Node", [](void *p, Args &a) {
    auto &nodes = *self<std::vector<lxb_dom_node_t *>>(p);
    if (a[0].i < 0 || static_cast<uint64_t>(a[0].i) >= nodes.size())
      throw dslvm::Error("NodeList.at index out of range");
    return Value::object("Node", nodes[static_cast<size_t>(a[0].i)]);
  });
  checked_method(r, "StrList", "len", {}, "int", [](void *p, Args &) {
    return Value::integer(static_cast<int64_t>(self<std::vector<std::string>>(p)->size()));
  });
  checked_method(r, "StrList", "at", {"int"}, "string", [](void *p, Args &a) {
    auto &items = *self<std::vector<std::string>>(p);
    if (a[0].i < 0 || static_cast<uint64_t>(a[0].i) >= items.size())
      throw dslvm::Error("StrList.at index out of range");
    return Value::string(items[static_cast<size_t>(a[0].i)]);
  });
  checked_method(r, "DocList", "len", {}, "int", [](void *p, Args &) {
    return Value::integer(static_cast<int64_t>(self<std::vector<lmhtml::Document *>>(p)->size()));
  });
  checked_method(r, "DocList", "at", {"int"}, "Doc", [](void *p, Args &a) {
    auto &docs = *self<std::vector<lmhtml::Document *>>(p);
    if (a[0].i < 0 || static_cast<uint64_t>(a[0].i) >= docs.size())
      throw dslvm::Error("DocList.at index out of range");
    return Value::object("Doc", docs[static_cast<size_t>(a[0].i)]);
  });
  // Site-wide aggregation: one call instead of the script-side str_union loop.
  auto aggregate = [&h, css_resolver](void *p, int what) {
    auto &pages = *self<std::vector<lmhtml::Document *>>(p);
    std::set<std::string> all;
    for (auto *doc : pages) {
      std::vector<std::string> names, other;
      switch (what) {
      case 0: names = lmhtml::classes_used(*doc); break;
      case 1: lmhtml::css_names(*doc, names, other, css_resolver(doc)); break;
      case 2: names = lmhtml::ids_used(*doc); break;
      case 3: lmhtml::css_names(*doc, other, names, css_resolver(doc)); break;
      }
      all.insert(names.begin(), names.end());
    }
    return h.hold(h.str_lists, "StrList", std::vector<std::string>(all.begin(), all.end()));
  };
  checked_method(r, "DocList", "classes_used", {}, "StrList", [aggregate](void *p, Args &) { return aggregate(p, 0); });
  checked_method(r, "DocList", "css_classes", {}, "StrList", [aggregate](void *p, Args &) { return aggregate(p, 1); });
  checked_method(r, "DocList", "ids", {}, "StrList", [aggregate](void *p, Args &) { return aggregate(p, 2); });
  checked_method(r, "DocList", "css_ids", {}, "StrList", [aggregate](void *p, Args &) { return aggregate(p, 3); });

  checked_method(r, "Html", "load", {"string"}, "Doc", [&h](void *, Args &a) {
    auto doc = lmhtml::parse_html(read_file(a[0].s), a[0].s);
    if (h.docs.size() >= 4096) throw dslvm::Error("Doc object limit exceeded (4096)");
    h.docs.push_back(std::move(doc));
    return Value::object("Doc", h.docs.back().get());
  });
  auto load_dir_impl = [&h](const std::string &dir, const std::string &include,
                            const std::string &exclude) {
    std::vector<lmhtml::Document *> pages;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
      throw dslvm::Error("Html.load_dir: not a directory: " + dir);
    const std::string include_pat = dir_pattern(dir, include, "include");
    const std::string exclude_pat = dir_pattern(dir, exclude, "exclude");
    std::vector<std::string> files;
    // Directory symlinks are not followed (no loops); unreadable directories
    // are a clean error rather than a silently partial site.
    std::filesystem::recursive_directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code entry_ec; // broken symlinks are skipped, not fatal
      if (!it->is_regular_file(entry_ec)) continue;
      std::string ext = it->path().extension().string();
      if (ext != ".html" && ext != ".htm") continue;
      // paths are normalized to forward slashes everywhere the DSL sees them
      std::string rel = it->path().lexically_relative(dir).generic_string();
      if (!include.empty() && include != "*" && !glob_path_match(rel, include_pat)) continue;
      if (!exclude.empty() && glob_path_match(rel, exclude_pat)) continue;
      files.push_back(it->path().generic_string());
    }
    if (ec) throw dslvm::Error("Html.load_dir: cannot list " + dir + ": " + ec.message());
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      if (h.docs.size() >= 4096) throw dslvm::Error("Doc object limit exceeded (4096)");
      h.docs.push_back(lmhtml::parse_html(read_file(file), file));
      pages.push_back(h.docs.back().get());
      h.dir_roots[pages.back()] = dir;
    }
    return h.hold(h.doc_lists, "DocList", std::move(pages));
  };
  checked_method(r, "Html", "load_dir", {"string"}, "DocList",
                 [load_dir_impl](void *, Args &a) { return load_dir_impl(a[0].s, "", ""); });
  checked_method(r, "Html", "load_dir", {"string", "string", "string"}, "DocList",
                 [load_dir_impl](void *, Args &a) {
                   return load_dir_impl(a[0].s, a[1].s, a[2].s);
                 });
  checked_method(r, "Html", "parse", {"string"}, "Doc", [&h](void *, Args &a) {
    try {
      if (h.docs.size() >= 4096) throw dslvm::Error("Doc object limit exceeded (4096)");
      h.docs.push_back(lmhtml::parse_html(a[0].s, ""));
    } catch (const std::string &e) {
      throw dslvm::Error("Html.parse: " + e);
    }
    return Value::object("Doc", h.docs.back().get());
  });
  checked_method(r, "Html", "save", {"Doc", "string"}, "void", [&h](void *, Args &a) {
    auto *doc = doc_of(a[0].obj, "Html.save");
    if (!doc->utf8_source)
      throw dslvm::Error("Html.save: " + (doc->path.empty() ? std::string("document") : doc->path) +
                         " is not valid UTF-8 (legacy encoding?); saving would mix encodings");
    const std::string &path = a[1].s;
    // Output root is the process cwd; saves must stay inside it. Inputs are
    // only modified when the script explicitly saves back to the same path.
    if (path.empty() || path.back() == '/')
      throw dslvm::Error("Html.save: path must name a file: " + path);
    std::error_code ec;
    // generic_string: the containment check compares against '/'
    std::string root =
        std::filesystem::weakly_canonical(std::filesystem::current_path(), ec).generic_string();
    std::string target =
        std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec).generic_string();
    if (ec || target.size() <= root.size() || target.compare(0, root.size(), root) != 0 ||
        target[root.size()] != '/')
      throw dslvm::Error("Html.save: path is outside the output root (working directory): " + path);
    std::string content;
    try {
      content = lmhtml::document_html(*doc);
    } catch (const std::string &e) {
      throw dslvm::Error("Html.save: " + e);
    }
    if (h.dry_run) {
      std::string state = "new";
      std::error_code ec2;
      if (std::filesystem::exists(target, ec2)) {
        try {
          state = read_file(target) == content ? "unchanged" : "changed";
        } catch (...) {
          state = "changed";
        }
      }
      h.report.findings.push_back(
          {"would-save", path,
           std::to_string(content.size()) + " bytes, " + state, 0});
      return Value::none();
    }
    write_inside_root(root, target, content, path);
    return Value::none();
  });
  checked_method(r, "Html", "resolve", {"Doc", "string"}, "string", [&h](void *, Args &a) {
    const std::string &url = a[1].s;
    LinkTarget t = resolve_link(h, doc_of(a[0].obj, "Html.resolve"), url);
    if (t.external || (t.same_page && t.file.empty())) return Value::string(url);
    return Value::string(t.file + (t.has_fragment ? "#" + t.fragment : ""));
  });
  checked_method(r, "Html", "link_kind", {"Doc", "string"}, "string", [&h](void *, Args &a) {
    return Value::string(link_kind(h, doc_of(a[0].obj, "Html.link_kind"), a[1].s));
  });
  checked_method(r, "Html", "link_exists", {"Doc", "string"}, "bool", [&h](void *, Args &a) {
    return Value::boolean(link_exists(h, doc_of(a[0].obj, "Html.link_exists"), a[1].s));
  });
  checked_method(r, "Html", "diff", {"Doc", "Doc"}, "int", [&h](void *, Args &a) {
    auto *da = doc_of(a[0].obj, "Html.diff");
    auto *db = doc_of(a[1].obj, "Html.diff");
    std::vector<lmhtml::DiffFinding> found;
    size_t n = lmhtml::diff_documents(*da, *db, found);
    for (const auto &f : found)
      h.report.findings.push_back({f.kind, f.file, f.message, f.line});
    return Value::integer(static_cast<int64_t>(n));
  });
  checked_method(r, "Html", "write_template", {"DocList", "int", "string"}, "int",
                 [&h](void *, Args &a) {
                   auto &pages = *self<std::vector<lmhtml::Document *>>(a[0].obj);
                   lmhtml::TemplateResult t = lmhtml::make_template(pages, a[1].i, a[2].s);
                   std::ofstream out(a[2].s, std::ios::binary);
                   if (!out) throw dslvm::Error("Html.write_template: cannot write " + a[2].s);
                   out << t.markup;
                   out.close();
                   if (!out) throw dslvm::Error("Html.write_template: failed writing " + a[2].s);
                   for (const auto &f : t.findings)
                     h.report.findings.push_back({f.kind, f.file, f.message, f.line});
                   return Value::integer(static_cast<int64_t>(t.slots));
                 });

  checked_method(r, "Report", "add", {"string", "string", "int", "string"}, "void",
                 [](void *p, Args &a) {
                   self<Report>(p)->findings.push_back({a[0].s, a[1].s, a[3].s, a[2].i});
                   return Value::none();
                 });
  checked_method(r, "Report", "write_text", {"string"}, "void", [](void *p, Args &a) {
    write_report_text(*self<Report>(p), a[0].s);
    return Value::none();
  });
  checked_method(r, "Report", "write_json", {"string"}, "void", [](void *p, Args &a) {
    write_report_json(*self<Report>(p), a[0].s);
    return Value::none();
  });

  checked_function(r, "suggest", {"string", "StrList"}, "StrList", [&h](Args &a) {
    auto &candidates = *self<std::vector<std::string>>(a[1].obj);
    return h.hold(h.str_lists, "StrList", lmhtml::strlib::suggest(a[0].s, candidates));
  });
  checked_function(r, "str_difference", {"StrList", "StrList"}, "StrList", [&h](Args &a) {
    auto &incl = *self<std::vector<std::string>>(a[0].obj);
    auto &excl = *self<std::vector<std::string>>(a[1].obj);
    return h.hold(h.str_lists, "StrList", lmhtml::strlib::difference(incl, excl));
  });
  checked_function(r, "str_union", {"StrList", "StrList"}, "StrList", [&h](Args &a) {
    auto &left = *self<std::vector<std::string>>(a[0].obj);
    auto &right = *self<std::vector<std::string>>(a[1].obj);
    return h.hold(h.str_lists, "StrList", lmhtml::strlib::set_union(left, right));
  });
  checked_function(r, "str_list", {}, "StrList",
                   [&h](Args &) { return h.hold(h.str_lists, "StrList", std::vector<std::string>{}); });
  checked_function(r, "str_starts_with", {"string", "string"}, "bool",
                   [](Args &a) { return Value::boolean(lmhtml::strlib::starts_with(a[0].s, a[1].s)); });
  checked_function(r, "str_ends_with", {"string", "string"}, "bool",
                   [](Args &a) { return Value::boolean(lmhtml::strlib::ends_with(a[0].s, a[1].s)); });
  checked_function(r, "str_contains", {"string", "string"}, "bool",
                   [](Args &a) { return Value::boolean(lmhtml::strlib::contains(a[0].s, a[1].s)); });
  checked_function(r, "str_lower", {"string"}, "string",
                   [](Args &a) { return Value::string(lmhtml::strlib::lower(a[0].s)); });
  checked_function(r, "str_substr", {"string", "int", "int"}, "string",
                   [](Args &a) { return Value::string(lmhtml::strlib::substr(a[0].s, a[1].i, a[2].i)); });
  checked_function(r, "str_len", {"string"}, "int",
                   [](Args &a) { return Value::integer(lmhtml::strlib::len(a[0].s)); });
  checked_function(r, "str_match", {"string", "string"}, "bool",
                   [](Args &a) { return Value::boolean(lmhtml::strlib::match(a[0].s, a[1].s)); });
}

// Short human/LLM-readable schema summary, mechanically derived from the
// registry's canonical signatures: context variables first, then free
// functions, then one line per member grouped by type.
void print_schema_summary(dslvm::Registry &reg) {
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> members;
  std::string current;
  for (const auto &signature : reg.signatures()) {
    if (signature.rfind("context ", 0) == 0) {
      std::cout << "context " << signature.substr(8) << "\n";
    } else if (signature.rfind("fn ", 0) == 0) {
      std::cout << "fn " << signature.substr(3) << "\n";
    } else if (signature.rfind("method ", 0) == 0) {
      std::string body = signature.substr(7);
      std::string type = body.substr(0, body.find('.'));
      if (type != current) {
        current = type;
        order.push_back(type);
      }
      members[type].push_back(body);
    }
  }
  for (const auto &type : order) {
    std::cout << type << ":\n";
    for (const auto &member : members[type]) std::cout << "  " << member << "\n";
  }
}

}
int main(int argc, char **argv) {
#ifdef _WIN32
  // MSVCRT text mode would translate \n to \r\n on stdout/stderr, breaking
  // --schema --json byte identity and the one-line JSON result protocol.
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  Host host;
  dslvm::Registry reg;
  register_host(reg, host);
  const std::string usage =
      "usage: lmhtml --schema [--json] | --dsl-signatures | [--dry-run] --dsl FILE [FILE ...]\n";
  if (argc >= 2 && std::string(argv[1]) == "--schema") {
    if (argc == 2) {
      print_schema_summary(reg);
      return std::cout ? 0 : 4;
    }
    if (argc == 3 && std::string(argv[2]) == "--json") {
      std::cout.write(reinterpret_cast<const char *>(lmhtml::embedded_schema),
                      static_cast<std::streamsize>(lmhtml::embedded_schema_size));
      return std::cout ? 0 : 4;
    }
    std::cerr << usage;
    return 2;
  }
  if (argc == 2 && std::string(argv[1]) == "--dsl-signatures") {
    for (const auto &signature : reg.signatures()) std::cout << signature << "\n";
    return 0;
  }
  int files_at = 2;
  if (argc >= 2 && std::string(argv[1]) == "--dry-run") {
    host.dry_run = true;
    files_at = 3;
  }
  if (argc <= files_at || std::string(argv[files_at - 1]) != "--dsl") {
    std::cerr << usage;
    return 2;
  }
  // Validate the complete invocation before any block can write output.
  for (int i = files_at; i < argc; i++) {
    if (std::string(argv[i]).empty() || std::string(argv[i]).rfind("--", 0) == 0) {
      std::cerr << usage;
      return 2;
    }
  }
  const std::string schema(reinterpret_cast<const char *>(lmhtml::embedded_schema),
                           lmhtml::embedded_schema_size);
  dslvm::Context context;
  context.set("html", &host);
  context.set("report", &host.report);
  // Load and verify every file first: a missing or corrupt later file must
  // not leave the earlier blocks' output half-written.
  std::vector<dslvm::Program> programs;
  for (int i = files_at; i < argc; i++) {
    try {
      programs.push_back(dslvm::Program::load(dslvm::read_file(argv[i]), schema, reg));
    } catch (const dslvm::Error &error) {
      return dslvm::report_error("load", error);
    }
  }
  for (const auto &program : programs) {
    try {
      program.run(context, reg);
    } catch (const dslvm::Error &error) {
      return dslvm::report_error("run", error);
    }
    dslvm::report_ok();
  }
  return 0;
}
