// Copyright 2013
// Author: Christopher Van Arsdale
//
// TODO(cvanarsdale): This overalaps a lot with go_library/cc_library.

#include <map>
#include <set>
#include <string>
#include <vector>
#include "common/log/log.h"
#include "common/file/fileutil.h"
#include "common/strings/path.h"
#include "common/strings/strutil.h"
#include "repobuild/env/input.h"
#include "repobuild/nodes/py_library.h"
#include "repobuild/reader/buildfile.h"

#include <json/json.h>

using std::map;
using std::set;
using std::string;
using std::vector;

namespace repobuild {

void PyLibraryNode::Parse(BuildFile* file, const BuildFileNode& input) {
  SimpleLibraryNode::Parse(file, input);

  // py_sources
  current_reader()->ParseRepeatedFiles("py_sources", &sources_);

  // py_base_dir
  vector<Resource> py_base_dirs;
  current_reader()->ParseSingleFile("py_base_dir",
                                    true,
                                    &py_base_dirs);
  if (!py_base_dirs.empty()) {
    if (py_base_dir_.size() > 1) {
      LOG(FATAL) << "Too many results for py_base_dir, need 1: "
                 << target().full_path();
    }
    py_base_dir_ = py_base_dirs[0].path() + "/";
  }

  Init();
}

void PyLibraryNode::LocalWriteMakeInternal(bool write_user_target,
                                           Makefile* out) const {
  SimpleLibraryNode::LocalWriteMake(out);

  // Move all go code into a single directory.
  vector<Resource> symlinked_sources;
  for (const Resource& source : sources_) {
    Resource symlink = PyFileFor(source);
    symlinked_sources.push_back(symlink);

    // Write main symlink for this file.
    out->WriteRootSymlinkWithDependency(
        symlink.path(),
        source.path(),
        strings::JoinPath(symlink.dirname(), "__init__.py"));
  }

  // Syntax check.
  string sources = strings::JoinAll(symlinked_sources, " ");
  Makefile::Rule* rule = out->StartRule(touchfile_.path(), sources);
  rule->WriteUserEcho("Compiling", target().full_path() + " (python)");
  rule->WriteCommand("python -m py_compile " + sources);
  rule->WriteCommand("mkdir -p " + Touchfile().dirname());
  rule->WriteCommand("touch " + Touchfile().path());
  out->FinishRule(rule);

  // User target.
  if (write_user_target) {
    ResourceFileSet deps;
    DependencyFiles(PYTHON, &deps);
    WriteBaseUserTarget(deps, out);
  }
}

void PyLibraryNode::LocalDependencyFiles(LanguageType lang,
                                         ResourceFileSet* files) const {
  SimpleLibraryNode::LocalDependencyFiles(lang, files);
  files->Add(touchfile_);
}

void PyLibraryNode::LocalObjectFiles(LanguageType lang,
                                     ResourceFileSet* files) const {
  for (const Resource& r : sources_) {
    files->Add(PyFileFor(r));
  }
}

void PyLibraryNode::ExternalDependencyFiles(
    LanguageType lang,
    map<string, string>* files) const {
  for (const Resource& r : sources_) {
    (*files)[PyFileFor(r).path()] = r.path();
  }
}

void PyLibraryNode::Set(const vector<Resource>& sources) {
  sources_ = sources;
  Init();
}

void PyLibraryNode::Init() {
  touchfile_ = Touchfile();
}

namespace {
bool IsBetterInitPy(const string& original, const string& replacement) {
  // TODO(cvanarsdale): __init__.py preference if multiple __init__.py
  // files map to the same location? This is a bit hacky.
  return replacement.size() < original.size();
}

void FillParentInitPy(map<string, string>* dir_to_init) {
  map<string, string> dir_copy = *dir_to_init;
  for (auto it : dir_copy) {
    string parent = strings::JoinPath(it.first, "../");
    string parent_init_py = strings::JoinPath(strings::PathDirname(it.second),
                                              "../__init__.py");
    while (parent != "." && !parent.empty() &&
           dir_copy.find(parent) == dir_copy.end() &&
           (dir_to_init->find(parent) == dir_to_init->end() ||
            IsBetterInitPy((*dir_to_init)[parent], parent_init_py))) {
      (*dir_to_init)[parent] = parent_init_py;
      parent = strings::PathDirname(strings::JoinPath(parent, "../"));
      parent_init_py = strings::JoinPath(parent, "__init__.py");
    }
  }
}

void InitialInitPyMapping(const map<string, string>& deps,
                          map<string, string>* dir_to_init) {
  for (auto it : deps) {
    if (strings::HasSuffix(it.first, ".py") ||
        strings::HasSuffix(it.first, ".pyc")) {
      string dirname = strings::PathDirname(it.first);
      string init_py = strings::JoinPath(strings::PathDirname(it.second),
                                         "__init__.py");
      if (dir_to_init->find(dirname) == dir_to_init->end() ||
          IsBetterInitPy((*dir_to_init)[dirname], init_py)) {
        (*dir_to_init)[dirname] = init_py;
      }
    }
  }
}

void FindExistingFiles(const set<string>& files, set<string>* actual) {
  vector<string> globbed;
  file::Glob(strings::JoinAll(files, " "), &globbed);
  for (const string& str : globbed) {
    LG << "FOUND: " << str;
    actual->insert(str);
  }
}

}  // anonymous namespace

// static
void PyLibraryNode::FinishMakeFile(const Input& input,
                                   const vector<const Node*>& all_nodes,
                                   Makefile* out) {
  // Find our python files.
  map<string, string> deps;
  for (const Node* node : all_nodes) {
    node->ExternalDependencyFiles(PYTHON, &deps);
  }

  // Figure out initial directory -> __init__.py mapping.
  map<string, string> dir_to_init;
  InitialInitPyMapping(deps, &dir_to_init);

  // Fill in unseen parent directories.
  FillParentInitPy(&dir_to_init);

  // Figure out which ones really exist.
  set<string> want;
  for (auto it : dir_to_init) {
    want.insert(it.second);
    want.insert(StripSpecialDirs(input, it.second));
  }
  set<string> have;
  FindExistingFiles(want, &have);

  // Fix up rules based on which actually exist, prefering normal source files
  // if they exist.
  for (auto& it : dir_to_init) {
    string stripped = StripSpecialDirs(input, it.second);
    if (have.find(stripped) != have.end()) {
      it.second = stripped;
    }
  }

  // Now output all symlink rules.
  for (auto it : dir_to_init) {
    const string& dir = it.first;
    const string& init_py = it.second;

    // The __init__.py file we are creating.
    Resource pkg_init_py = Resource::FromLocalPath(
        dir, "__init__.py");

    // The __init__.py file in our parent directory, if any
    Resource parent_pkg_init_py = Resource::FromLocalPath(
        dir, "../__init__.py");
    string parent;
    if (dir_to_init.find(parent_pkg_init_py.dirname()) != dir_to_init.end()) {
      parent = parent_pkg_init_py.path();
    }

    // Our symlink.
    out->WriteRootSymlinkWithDependency(
        pkg_init_py.path(),
        init_py,
        parent);
  }
}

Resource PyLibraryNode::PyFileFor(const Resource& r) const {
  string file = StripSpecialDirs(r.path());
  if (strings::HasPrefix(file, py_base_dir_)) {
    file = file.substr(py_base_dir_.size());
  }
  return Resource::FromLocalPath(input().pkgfile_dir(), file);
}

}  // namespace repobuild