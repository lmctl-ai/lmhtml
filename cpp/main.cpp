#include "dslvm.hpp"
#include "html.hpp"
#include "schema_embedded.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
  template <class T>
  Value hold(std::vector<std::unique_ptr<T>> &arena, const char *type, T value) {
    if (arena.size() >= 4096)
      throw dslvm::Error(std::string(type) + " object limit exceeded (4096)");
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

std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  return out;
}

void write_report_text(const Report &report, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw dslvm::Error("cannot write " + path);
  for (const auto &f : report.findings)
    out << f.kind << " " << f.file << ":" << f.line << ": " << f.message << "\n";
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

// link_exists policy (v0): external schemes and non-file schemes are
// unchecked and count as existing; relative links resolve against the
// page's own path; a #fragment must match an id or a[name] in the target.
bool link_exists(Host &h, lmhtml::Document *doc, const std::string &url) {
  if (url.empty() || !lmhtml::url_scheme(url).empty()) return true;
  std::string file = url, frag;
  std::string::size_type hash = url.find('#');
  if (hash != std::string::npos) {
    file = url.substr(0, hash);
    frag = url.substr(hash + 1);
  }
  lmhtml::Document *target = doc;
  std::unique_ptr<lmhtml::Document> loaded;
  if (!file.empty()) {
    std::string resolved = lmhtml::resolve_relative(doc->path, file);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) return false;
    if (frag.empty()) return true;
    if (resolved == doc->path) {
      target = doc;
    } else {
      try {
        loaded = lmhtml::parse_html(read_file(resolved), resolved);
      } catch (const dslvm::Error &) {
        throw;
      } catch (const std::string &e) {
        throw dslvm::Error(e);
      }
      target = loaded.get();
    }
  }
  (void) h;
  return frag.empty() || lmhtml::has_anchor(*target, frag);
}

Value not_yet(const char *name, const char *milestone) {
  throw dslvm::Error(std::string(name) + " is not implemented yet (" + milestone + ")");
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
  checked_method(r, "Doc", "classes_used", {}, "StrList",
                 [](void *, Args &) { return not_yet("Doc.classes_used", "milestone M2"); });
  checked_method(r, "Doc", "css_classes", {}, "StrList",
                 [](void *, Args &) { return not_yet("Doc.css_classes", "milestone M2"); });
  checked_method(r, "Doc", "ids", {}, "StrList",
                 [](void *, Args &) { return not_yet("Doc.ids", "milestone M2"); });

  checked_method(r, "Node", "tag", {}, "string", [](void *p, Args &) {
    return Value::string(lmhtml::tag_name(node_of(p, "Node.tag")));
  });
  checked_method(r, "Node", "attr", {"string"}, "string", [](void *p, Args &a) {
    return Value::string(lmhtml::attr(node_of(p, "Node.attr"), a[0].s));
  });
  checked_method(r, "Node", "has_attr", {"string"}, "bool", [](void *p, Args &a) {
    return Value::boolean(lmhtml::has_attr(node_of(p, "Node.has_attr"), a[0].s));
  });
  checked_method(r, "Node", "set_attr", {"string", "string"}, "Node",
                 [](void *, Args &) { return not_yet("Node.set_attr", "milestone M5"); });
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
  checked_method(r, "Node", "remove", {}, "void",
                 [](void *, Args &) { return not_yet("Node.remove", "milestone M5"); });
  checked_method(r, "Node", "replace_with", {"string"}, "Node",
                 [](void *, Args &) { return not_yet("Node.replace_with", "milestone M5"); });
  checked_method(r, "Node", "hash", {}, "int",
                 [](void *, Args &) { return not_yet("Node.hash", "milestone M3"); });

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

  checked_method(r, "Html", "load", {"string"}, "Doc", [&h](void *, Args &a) {
    auto doc = lmhtml::parse_html(read_file(a[0].s), a[0].s);
    if (h.docs.size() >= 4096) throw dslvm::Error("Doc object limit exceeded (4096)");
    h.docs.push_back(std::move(doc));
    return Value::object("Doc", h.docs.back().get());
  });
  checked_method(r, "Html", "load_dir", {"string"}, "DocList", [&h](void *, Args &a) {
    std::vector<lmhtml::Document *> pages;
    std::error_code ec;
    if (!std::filesystem::is_directory(a[0].s, ec))
      throw dslvm::Error("Html.load_dir: not a directory: " + a[0].s);
    std::vector<std::string> files;
    // Directory symlinks are not followed (no loops); unreadable directories
    // are a clean error rather than a silently partial site.
    std::filesystem::recursive_directory_iterator it(a[0].s, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code entry_ec; // broken symlinks are skipped, not fatal
      if (!it->is_regular_file(entry_ec)) continue;
      std::string ext = it->path().extension().string();
      if (ext == ".html" || ext == ".htm") files.push_back(it->path().string());
    }
    if (ec) throw dslvm::Error("Html.load_dir: cannot list " + a[0].s + ": " + ec.message());
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
      if (h.docs.size() >= 4096) throw dslvm::Error("Doc object limit exceeded (4096)");
      h.docs.push_back(lmhtml::parse_html(read_file(file), file));
      pages.push_back(h.docs.back().get());
    }
    return h.hold(h.doc_lists, "DocList", std::move(pages));
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
  checked_method(r, "Html", "save", {"Doc", "string"}, "void",
                 [](void *, Args &) { return not_yet("Html.save", "milestone M5"); });
  checked_method(r, "Html", "resolve", {"Doc", "string"}, "string", [](void *, Args &a) {
    auto *doc = doc_of(a[0].obj, "Html.resolve");
    const std::string &url = a[1].s;
    if (url.empty() || !lmhtml::url_scheme(url).empty() || url[0] == '#')
      return Value::string(url);
    std::string frag;
    std::string file = url;
    std::string::size_type hash = url.find('#');
    if (hash != std::string::npos) {
      file = url.substr(0, hash);
      frag = url.substr(hash);
    }
    return Value::string(lmhtml::resolve_relative(doc->path, file) + frag);
  });
  checked_method(r, "Html", "link_exists", {"Doc", "string"}, "bool", [&h](void *, Args &a) {
    return Value::boolean(link_exists(h, doc_of(a[0].obj, "Html.link_exists"), a[1].s));
  });
  checked_method(r, "Html", "diff", {"Doc", "Doc"}, "int",
                 [](void *, Args &) { return not_yet("Html.diff", "milestone M3"); });
  checked_method(r, "Html", "write_template", {"DocList", "int", "string"}, "int",
                 [](void *, Args &) { return not_yet("Html.write_template", "milestone M4"); });

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

  checked_function(r, "suggest", {"string", "StrList"}, "StrList",
                   [](Args &) { return not_yet("suggest", "milestone M2"); });
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
  Host host;
  dslvm::Registry reg;
  register_host(reg, host);
  const std::string usage =
      "usage: lmhtml --schema [--json] | --dsl-signatures | --dsl FILE [FILE ...]\n";
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
  if (argc < 3 || std::string(argv[1]) != "--dsl") {
    std::cerr << usage;
    return 2;
  }
  // Validate the complete invocation before any block can write output.
  for (int i = 2; i < argc; i++) {
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
  for (int i = 2; i < argc; i++) {
    dslvm::Program program;
    try {
      program = dslvm::Program::load(dslvm::read_file(argv[i]), schema, reg);
    } catch (const dslvm::Error &error) {
      return dslvm::report_error("load", error);
    }
    try {
      program.run(context, reg);
    } catch (const dslvm::Error &error) {
      return dslvm::report_error("run", error);
    }
    dslvm::report_ok();
  }
  return 0;
}
