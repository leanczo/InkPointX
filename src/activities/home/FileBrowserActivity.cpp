#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FavoriteBooksStore.h"
#include "FileActionsActivity.h"
#include "FileInfoActivity.h"
#include "FolderPickerActivity.h"
#include "MappedInputManager.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;

bool isPathInside(const std::string& path, const std::string& parent) {
  return path == parent ||
         (path.size() > parent.size() && path.compare(0, parent.size(), parent) == 0 && path[parent.size()] == '/');
}
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if (strcmp(fileNameBuffer.get(), ".crosspoint") == 0 || (!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (mode == Mode::PickSleepImage) {
        if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
            FsHelpers::hasPngExtension(filename)) {
          files.emplace_back(filename);
        }
      } else if (mode == Mode::Manager) {
        files.emplace_back(filename);
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasFb2Extension(filename) || FsHelpers::hasPdfExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;
  if (basepath.compare(0, strlen("/.crosspoint"), "/.crosspoint") == 0) basepath = "/";

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

std::string FileBrowserActivity::joinPath(const std::string& directory, const std::string& name) {
  if (directory == "/") return "/" + name;
  return directory.back() == '/' ? directory + name : directory + "/" + name;
}

std::string FileBrowserActivity::baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool FileBrowserActivity::isValidName(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name == ".crosspoint" || name == "System Volume Information" ||
      name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
    return false;
  }
  constexpr std::string_view invalid = R"(<>:"/\|?*)";
  return std::none_of(name.begin(), name.end(), [](const unsigned char c) { return c < 32; }) &&
         name.find_first_of(invalid) == std::string::npos;
}

void FileBrowserActivity::showOperationMessage(const char* message) {
  GUI.drawPopup(renderer, message);
  delay(750);
  requestUpdate(true);
}

bool FileBrowserActivity::copyFile(const std::string& sourcePath, const std::string& destinationPath) {
  if (Storage.exists(destinationPath.c_str())) return false;
  const std::string temporaryPath = destinationPath + ".inkpoint-copy.tmp";
  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) return false;

  HalFile source;
  HalFile destination;
  if (!Storage.openFileForRead("FILECOPY", sourcePath, source) ||
      !Storage.openFileForWrite("FILECOPY", temporaryPath, destination)) {
    if (source) source.close();
    if (destination) destination.close();
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  bool success = true;
  std::array<uint8_t, 1024> buffer{};
  while (true) {
    const int count = source.read(buffer.data(), buffer.size());
    if (count < 0) {
      success = false;
      break;
    }
    if (count == 0) break;
    if (destination.write(buffer.data(), static_cast<size_t>(count)) != static_cast<size_t>(count)) {
      success = false;
      break;
    }
  }
  source.close();
  destination.close();

  if (!success || !Storage.rename(temporaryPath.c_str(), destinationPath.c_str())) {
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  return true;
}

bool FileBrowserActivity::copyDirFile(const std::string& sourcePath, const std::string& destinationDirectory) {
  auto source = Storage.open(sourcePath.c_str());
  if (!source) return false;
  const bool sourceIsDirectory = source.isDirectory();
  source.close();

  if (sourceIsDirectory && isPathInside(destinationDirectory, sourcePath)) {
    return false;
  }

  const std::string destinationPath = joinPath(destinationDirectory, baseName(sourcePath));
  if (Storage.exists(destinationPath.c_str())) return false;
  if (!sourceIsDirectory) return copyFile(sourcePath, destinationPath);
  if (!Storage.mkdir(destinationPath.c_str(), true)) return false;

  std::vector<std::pair<std::string, std::string>> stack;
  stack.reserve(16);
  stack.push_back({sourcePath, destinationPath});
  bool success = true;

  while (!stack.empty() && success) {
    auto [sourceDirectory, destinationDirectoryPath] = std::move(stack.back());
    stack.pop_back();

    auto directory = Storage.open(sourceDirectory.c_str());
    if (!directory || !directory.isDirectory()) {
      success = false;
      break;
    }
    directory.rewindDirectory();

    for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        entry.close();
        continue;
      }
      const bool isDirectory = entry.isDirectory();
      entry.close();

      const std::string childSource = joinPath(sourceDirectory, name);
      const std::string childDestination = joinPath(destinationDirectoryPath, name);
      if (isDirectory) {
        if (!Storage.mkdir(childDestination.c_str(), true)) {
          success = false;
          break;
        }
        stack.push_back({childSource, childDestination});
      } else if (!copyFile(childSource, childDestination)) {
        success = false;
        break;
      }
    }
    directory.close();
  }

  if (!success) removeDirFile(destinationPath);
  return success;
}

bool FileBrowserActivity::moveDirFile(const std::string& sourcePath, const std::string& destinationDirectory) {
  auto source = Storage.open(sourcePath.c_str());
  if (!source) return false;
  const bool sourceIsDirectory = source.isDirectory();
  source.close();
  if (sourceIsDirectory && isPathInside(destinationDirectory, sourcePath)) {
    return false;
  }

  const std::string destinationPath = joinPath(destinationDirectory, baseName(sourcePath));
  if (Storage.exists(destinationPath.c_str())) return false;
  return renameDirFile(sourcePath, destinationPath);
}

void FileBrowserActivity::migrateBookState(const std::string& oldPath, const std::string& newPath) {
  const std::string oldCachePath = getBookCachePath(oldPath);
  const std::string newCachePath = getBookCachePath(newPath);
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str()) && !Storage.exists(newCachePath.c_str()) &&
      !Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
    LOG_ERR("FileBrowser", "Failed to move cache: %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
  }
  RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  FAVORITE_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
}

bool FileBrowserActivity::renameDirFile(const std::string& sourcePath, const std::string& destinationPath) {
  auto source = Storage.open(sourcePath.c_str());
  if (!source) return false;
  const bool sourceIsDirectory = source.isDirectory();
  source.close();
  if (!Storage.rename(sourcePath.c_str(), destinationPath.c_str())) return false;

  if (!sourceIsDirectory) {
    migrateBookState(sourcePath, destinationPath);
    return true;
  }

  std::vector<std::string> directories;
  directories.reserve(16);
  directories.push_back(destinationPath);
  while (!directories.empty()) {
    std::string directoryPath = std::move(directories.back());
    directories.pop_back();
    auto directory = Storage.open(directoryPath.c_str());
    if (!directory || !directory.isDirectory()) continue;
    directory.rewindDirectory();
    for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        entry.close();
        continue;
      }
      const bool isDirectory = entry.isDirectory();
      entry.close();
      const std::string newChildPath = joinPath(directoryPath, name);
      if (isDirectory) {
        directories.push_back(newChildPath);
        continue;
      }
      if (getBookCachePath(newChildPath).empty()) continue;
      const std::string oldChildPath = sourcePath + newChildPath.substr(destinationPath.size());
      migrateBookState(oldChildPath, newChildPath);
    }
    directory.close();
  }
  return true;
}

void FileBrowserActivity::createFolder() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult || !isValidName(keyboardResult->text)) {
      showOperationMessage(tr(STR_NAME_INVALID));
      return;
    }
    const std::string path = joinPath(basepath, keyboardResult->text);
    if (Storage.exists(path.c_str())) {
      showOperationMessage(tr(STR_ALREADY_EXISTS));
      return;
    }
    if (!Storage.mkdir(path.c_str(), false)) {
      showOperationMessage(tr(STR_FAILED_LOWER));
      return;
    }
    loadFiles();
    selectorIndex = findEntry(keyboardResult->text + "/");
    showOperationMessage(tr(STR_FOLDER_CREATED));
  };
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FOLDER_NAME), "", 96),
                         handler);
}

void FileBrowserActivity::renameEntry(const std::string& fullPath) {
  const std::string oldName = baseName(fullPath);
  auto handler = [this, fullPath, oldName](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* keyboardResult = std::get_if<KeyboardResult>(&result.data);
    if (!keyboardResult || !isValidName(keyboardResult->text)) {
      showOperationMessage(tr(STR_NAME_INVALID));
      return;
    }
    if (keyboardResult->text == oldName) {
      requestUpdate(true);
      return;
    }
    const std::string newPath = joinPath(basepath, keyboardResult->text);
    if (Storage.exists(newPath.c_str())) {
      showOperationMessage(tr(STR_ALREADY_EXISTS));
      return;
    }
    if (!renameDirFile(fullPath, newPath)) {
      showOperationMessage(tr(STR_RENAME_FAILED));
      return;
    }
    loadFiles();
    auto renamed = Storage.open(newPath.c_str());
    const bool directory = renamed && renamed.isDirectory();
    if (renamed) renamed.close();
    selectorIndex = findEntry(keyboardResult->text + (directory ? "/" : ""));
    showOperationMessage(tr(STR_DONE));
  };
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_RENAME), oldName, 96),
                         handler);
}

void FileBrowserActivity::deleteEntry(const std::string& fullPath, const std::string& entry) {
  auto handler = [this, fullPath](const ActivityResult& result) {
    if (result.isCancelled) return;
    if (!removeDirFile(fullPath)) {
      showOperationMessage(tr(STR_FAILED_LOWER));
      return;
    }
    if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
    FAVORITE_BOOKS.pruneMissing();
    loadFiles();
    if (files.empty()) {
      selectorIndex = 0;
    } else if (selectorIndex >= files.size()) {
      selectorIndex = files.size() - 1;
    }
    showOperationMessage(tr(STR_DONE));
  };
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE) + std::string("?"), entry), handler);
}

void FileBrowserActivity::chooseDestination(const std::string& fullPath, const bool move) {
  auto handler = [this, fullPath, move](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* pathResult = std::get_if<FilePathResult>(&result.data);
    if (!pathResult) return;
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    const bool success = move ? moveDirFile(fullPath, pathResult->path) : copyDirFile(fullPath, pathResult->path);
    if (success) {
      loadFiles();
      if (selectorIndex >= files.size() && !files.empty()) selectorIndex = files.size() - 1;
      showOperationMessage(tr(STR_DONE));
    } else {
      showOperationMessage(move ? tr(STR_MOVE_FAILED) : tr(STR_COPY_FAILED));
    }
  };
  startActivityForResult(std::make_unique<FolderPickerActivity>(renderer, mappedInput, basepath), handler);
}

void FileBrowserActivity::openEntry(const std::string& fullPath, const bool isDirectory) {
  if (isDirectory) {
    basepath = fullPath;
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (FsHelpers::hasBmpExtension(fullPath) || FsHelpers::hasJpgExtension(fullPath) ||
      FsHelpers::hasPngExtension(fullPath)) {
    startActivityForResult(std::make_unique<BmpViewerActivity>(renderer, mappedInput, fullPath, true),
                           [this](const ActivityResult&) { requestUpdate(true); });
    return;
  }

  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath) ||
      FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath) ||
      FsHelpers::hasFb2Extension(fullPath) || FsHelpers::hasPdfExtension(fullPath)) {
    onSelectBook(fullPath);
    return;
  }

  auto file = Storage.open(fullPath.c_str());
  const uint64_t size = file ? file.fileSize64() : 0;
  if (file) file.close();
  startActivityForResult(std::make_unique<FileInfoActivity>(renderer, mappedInput, fullPath, false, size),
                         [this](const ActivityResult&) { requestUpdate(true); });
}

void FileBrowserActivity::showActions(const std::string& entry) {
  const bool isDirectory = entry.back() == '/';
  const std::string cleanEntry = isDirectory ? entry.substr(0, entry.size() - 1) : entry;
  const std::string fullPath = joinPath(basepath, cleanEntry);
  auto handler = [this, fullPath, entry, isDirectory](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* menuResult = std::get_if<MenuResult>(&result.data);
    if (!menuResult) return;
    const auto action = static_cast<FileActionsActivity::Action>(menuResult->action);
    switch (action) {
      case FileActionsActivity::Action::Open:
        openEntry(fullPath, isDirectory);
        break;
      case FileActionsActivity::Action::NewFolder:
        createFolder();
        break;
      case FileActionsActivity::Action::Copy:
        chooseDestination(fullPath, false);
        break;
      case FileActionsActivity::Action::Move:
        chooseDestination(fullPath, true);
        break;
      case FileActionsActivity::Action::Rename:
        renameEntry(fullPath);
        break;
      case FileActionsActivity::Action::Delete:
        deleteEntry(fullPath, entry);
        break;
      case FileActionsActivity::Action::Properties: {
        auto file = Storage.open(fullPath.c_str());
        const uint64_t size = file && !isDirectory ? file.fileSize64() : 0;
        if (file) file.close();
        startActivityForResult(std::make_unique<FileInfoActivity>(renderer, mappedInput, fullPath, isDirectory, size),
                               [this](const ActivityResult&) { requestUpdate(true); });
        break;
      }
    }
  };
  startActivityForResult(std::make_unique<FileActionsActivity>(renderer, mappedInput, cleanEntry), handler);
}

void FileBrowserActivity::loop() {
  // Long press BACK (1s+) goes to root folder (manager mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Manager && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) {
      if (mode == Mode::Manager) createFolder();
      return;
    }

    const std::string& entry = files[selectorIndex];
    const bool isDirectory = (entry.back() == '/');
    const std::string fullPath = joinPath(basepath, isDirectory ? entry.substr(0, entry.size() - 1) : entry);

    // Picker modes: select file -> return path; navigate into directories normally.
    if (mode != Mode::Manager && !isDirectory) {
      ActivityResult res{FilePathResult{fullPath}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Manager && mappedInput.getHeldTime() >= GO_HOME_MS) {
      showActions(entry);
      return;
    }

    openEntry(fullPath, isDirectory);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode != Mode::Manager) {
        // Pickers at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  if (listSize > 0) {
    buttonNavigator.onNextPress([this, listSize] {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    });

    buttonNavigator.onPreviousPress([this, listSize] {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
      requestUpdate();
    });

    buttonNavigator.onNextContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      requestUpdate();
    });
  }
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  // A leading dot is part of the name, not an extension separator: splitting on
  // it left ".nomedia" with an empty title, so the row rendered blank yet stayed
  // selectable and deletable.
  if (pos == 0 || pos == std::string::npos) return filename;
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  // Keep the two halves consistent: a dotfile's whole name is the title, so it
  // must not also be reported as the extension.
  if (pos == 0 || pos == std::string::npos) return "";
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName;
  if (mode == Mode::PickFirmware) {
    folderName = tr(STR_SELECT_FIRMWARE_FILE);
  } else if (mode == Mode::PickSleepImage) {
    folderName = tr(STR_LOCK_SCREEN_IMAGE);
  } else if (mode == Mode::PickBook) {
    folderName = tr(STR_OPEN_FROM_FILE);
  } else {
    folderName = (basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1);
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    GUI.drawEmptyState(renderer, Rect{0, contentTop, pageWidth, contentHeight}, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    // A 3 px edge-to-edge rule was the heaviest element on the screen and read
    // as a hardware artifact; use the shared inset hairline instead.
    GUI.drawDivider(renderer, metrics.contentSidePadding, pageWidth - metrics.contentSidePadding - 1, separatorY);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode != Mode::Manager ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // Confirm on a picker file returns its path; directories still descend.
  const bool selectingFile = mode != Mode::Manager && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty()           ? (mode == Mode::Manager ? tr(STR_NEW_FOLDER) : "")
                             : selectingFile         ? tr(STR_SELECT)
                             : mode == Mode::Manager ? tr(STR_OPEN_ACTIONS)
                                                     : tr(STR_OPEN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
