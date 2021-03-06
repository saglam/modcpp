#include "org/modcpp/bluckbuild/BluckBuild.h"

#include <string>
#include <vector>
#include <unordered_set>

#include "org/modcpp/bluckbuild/BluckEnvironment.h"
#include "org/modcpp/bluckbuild/CppExecutor.h"
#include "org/modcpp/bluckbuild/Parser.h"
#include "org/modcpp/bluckbuild/Stamper.h"
#include "org/modcpp/bluckbuild/Target.h"
#include "org/modcpp/io/Files.h"
#include "org/modcpp/logging/Console.h"
#include "org/modcpp/string/Cstrings.h"

namespace org::modcpp::bluckbuild {

  using org::modcpp::logging::Console;
  using org::modcpp::io::Files;
  using std::string;
  using std::unordered_set;
  using std::vector;

  BluckBuild::BluckBuild() {
    environment.readConfigFile();
    Files::ensureDirectory(environment.getBinFolderName().c_str(),
        environment.getBluckRootPath().c_str());
    failedTests = 0;
    passedTests = 0;
    freshTests = 0;
  }

  BluckBuild::Result BluckBuild::cleanTarget(const string &path) {
    return Result::Success;
  }

  BluckBuild::Result BluckBuild::buildTarget(const string &path) {
    vector<string> depPaths;
    Result buildResult = processTargetRecursive(
        readTarget(environment.translatePath(path)), false, depPaths);

    switch (buildResult) {
      case BluckBuild::Result::Fresh:
        Console::info("Target is up to date.");
        break;
      case BluckBuild::Result::Success:
        Console::info("Target succesfully built.");
        break;
      case BluckBuild::Result::Fail:
        Console::error("Build error.");
        break;
    }
    return buildResult;
  }

  BluckBuild::Result BluckBuild::runTarget(const string &path) {
    Target target = readTarget(environment.translatePath(path));
    assert(target.getArtifact() != Target::Artifact::Library);

    vector<string> depPaths;
    Result buildResult = processTargetRecursive(target, false, depPaths);
    if (buildResult == Result::Fail) {
      Console::warning("Build failed. Trying the stale binary.");
    }

    switch (target.getLanguage()) {
      case Target::Language::Cpp: {
        CppExecutor cppExecutor(environment);
        return cppExecutor.run(target);
      }
      default:
        Console::error("Not supported");
        return Result::Fail;
    }
  }

  BluckBuild::Result BluckBuild::testTarget(const string &path) {
    Target target = readTarget(environment.translatePath(path));
    if (target.getArtifact() != Target::Artifact::Test) {
      Console::error("Target is not a test target");
      return Result::Fail;
    }

    vector<string> depPaths;
    Result result = processTargetRecursive(target, true, depPaths);
    if (result == Result::Fail) {
      Console::error("Build failed");
    }
    
    Console::info("%d fresh", freshTests);
    Console::info("%d passed", passedTests);
    if (failedTests != 0) {
      Console::error("%d failed", failedTests);
      result = Result::Fail;
    }
    Console::info("%d total tests", freshTests + passedTests + failedTests);

    return result;
  }

  Target BluckBuild::readTarget(const string &bluckPath) const {
    Target target;
    auto targetSeparator = bluckPath.rfind(":");
    if (targetSeparator == string::npos) {
      target.name = bluckPath.substr(bluckPath.rfind("/") + 1);
      target.package = bluckPath;
    } else {
      target.name = bluckPath.substr(targetSeparator + 1);
      target.package = bluckPath.substr(0, targetSeparator);
    }

    if (target.package == "//system/package") {
      target.type = Target::Type::External;
    } else {
      string fileName = environment.getBluckRootPath() + target.package.substr(1) + "/BLUCK";
      Parser parser(fileName);
      bool isSuccess = parser.populateTarget(target);

      if (!isSuccess) {
        Console::error("Parsing failed");
        exit(0);
      }
    }
    return target;
  }

  BluckBuild::Result BluckBuild::buildTargetSelf(const Target &target, bool isTest,
      const vector<string> &depPaths) const {
    if (target.getArtifact() == Target::Artifact::Test and target.srcs.empty()) {
      return Result::Fresh;
    }
    /*if (target.isExternal()) {
      PackageExecutor packageExecutor(environment);
      return packageExecutor.build(target);
    }*/
    switch (target.getLanguage()) {
      case Target::Language::Cpp: {
        CppExecutor cppExecutor(environment);
        return cppExecutor.build(target, isTest, depPaths);
      }
      default:
        Console::error("Not yet supported.");
        return Result::Fail;
    }
  }

  BluckBuild::Result BluckBuild::testTargetSelf(const Target &target) const {
    switch (target.getLanguage()) {
      case Target::Language::Cpp: {
        CppExecutor cppExecutor(environment);
        return cppExecutor.test(target);
      }
      default:
        Console::error("Not supported");
        return Result::Fail;
    }
  }

  BluckBuild::Result BluckBuild::processTargetRecursive(const Target &target, bool isTest,
      vector<string> &parentDepPaths) {
    string targetPath = target.getBluckPath();
    Files::ensureDirectory(target.package.substr(1).c_str(),
       environment.getBinFolderPath().c_str());

    // Check for dependency cycles
    if (parentTargetPaths.find(targetPath) != parentTargetPaths.end()) {
      Console::error("Cyclic dependency; Cannot build %s", targetPath.c_str());
      for (auto &path : parentTargetPaths) {
        Console::info("%s", path.c_str());
      }
      return Result::Fail;
    }

    // Check if we have already built this target in the current invocation.
    if (builtTargetPaths.find(targetPath) != builtTargetPaths.end()) {
      return Result::Fresh;
    }
    parentTargetPaths.insert(targetPath);
    builtTargetPaths.insert(targetPath);

    Console::info("Building target %s", targetPath.c_str());

    // Recursively build dependencies
    Result depsResult = Result::Fresh;
    unordered_set<string> parentDepPathsSet;
    vector<string> depPaths;
    for (const string &depPath : target.deps) {
      Target depTarget = readTarget(depPath);
      Result depResult = processTargetRecursive(depTarget, isTest, depPaths);
      if (depResult == Result::Fail) {
        return Result::Fail;
      }
      // TODO(saglam): Handle target merging more generically
      if (depTarget.getArtifact() == Target::Artifact::Library) {
        if (depResult > depsResult) {
          depsResult = depResult;
        }      

        for (const string &depFromChild : depPaths) {
          if (parentDepPathsSet.find(depFromChild) == parentDepPathsSet.end()) {
            parentDepPathsSet.insert(depFromChild);
            parentDepPaths.push_back(depFromChild);
          }
        }
      } else Console::info("NOT %s", target.name.c_str());
      depPaths.clear();
    }

    Stamper stamper(environment);
    Result selfResult = depsResult;
    if (depsResult != Result::Fresh || !stamper.isStampFresh(target)) {
      Result compileResult = buildTargetSelf(target, isTest, parentDepPaths);
      if (compileResult == Result::Fail) {
        Console::error("Compilation failed for target %s", targetPath.c_str());
        return Result::Fail;
      } else if (compileResult == Result::Fresh && depsResult != Result::Fresh) {
        Console::warning("You have unused dependencies at target %s", targetPath.c_str());
      }
      selfResult = Result::Success;
      stamper.applyStamp(target);
    }

    if (isTest && target.getArtifact() == Target::Artifact::Test
        and not target.srcs.empty()) {
      if (stamper.checkTestedMark(target)) {
        freshTests++;
      } else {
        Result testResult = testTargetSelf(target);
        if (testResult == Result::Fail) {
          Console::error("Test target %s failed", targetPath.c_str());
          failedTests++;
        } else {
          stamper.markTested(target);
          passedTests++;
        }
      }
    }

    parentDepPaths.push_back(targetPath);
    parentTargetPaths.erase(targetPath);
    return selfResult;
  }

} // namespace
