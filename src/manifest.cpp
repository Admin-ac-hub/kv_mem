#include "manifest.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include "format.h"

namespace kv {

namespace {

constexpr int kManifestVersion = 1;
constexpr const char* kManifestFileName = "MANIFEST";
constexpr const char* kCurrentFileName = "CURRENT";

std::filesystem::path CurrentPath(const std::filesystem::path& db_path) {
  return db_path / kCurrentFileName;
}

std::map<std::uint64_t, SSTableMeta> FileMap(const std::vector<SSTableMeta>& files) {
  std::map<std::uint64_t, SSTableMeta> by_number;
  for (const auto& file : files) {
    by_number[file.file_number] = file;
  }
  return by_number;
}

std::string BuildEditPayload(const VersionEdit& persisted, const VersionEdit& next) {
  std::ostringstream out;
  out << "version_edit " << kManifestVersion << '\n';
  out << "next_file_number " << next.next_file_number << '\n';
  out << "wal_file_number " << next.wal_file_number << '\n';
  out << "last_sequence " << next.last_sequence << '\n';

  const auto old_files = FileMap(persisted.sstables);
  const auto new_files = FileMap(next.sstables);
  for (const auto& item : old_files) {
    if (new_files.find(item.first) == new_files.end()) {
      out << "delete_file " << item.first << '\n';
    }
  }
  for (const auto& item : new_files) {
    if (old_files.find(item.first) == old_files.end()) {
      out << "add_file " << item.second.file_number << ' '
          << item.second.file_path.filename().string() << ' '
          << item.second.level << ' '
          << std::quoted(item.second.smallest_key) << ' '
          << std::quoted(item.second.largest_key) << ' '
          << item.second.file_size << '\n';
    }
  }
  return out.str();
}

Status ParseEditPayload(const std::filesystem::path& db_path,
                        const std::string& payload,
                        VersionEdit* edit) {
  std::istringstream parsed(payload);
  bool saw_version = false;
  std::string line;
  while (std::getline(parsed, line)) {
    if (line.empty()) {
      continue;
    }
    std::istringstream line_stream(line);
    std::string tag;
    line_stream >> tag;
    if (tag == "version_edit") {
      int version = 0;
      if (!(line_stream >> version) || version != kManifestVersion) {
        return Status::Corruption("invalid MANIFEST edit version");
      }
      saw_version = true;
    } else if (tag == "next_file_number") {
      if (!(line_stream >> edit->next_file_number)) {
        return Status::Corruption("invalid MANIFEST edit next_file_number");
      }
    } else if (tag == "wal_file_number") {
      if (!(line_stream >> edit->wal_file_number)) {
        return Status::Corruption("invalid MANIFEST edit wal_file_number");
      }
    } else if (tag == "last_sequence") {
      if (!(line_stream >> edit->last_sequence)) {
        return Status::Corruption("invalid MANIFEST edit last_sequence");
      }
    } else if (tag == "delete_file") {
      std::uint64_t file_number = 0;
      if (!(line_stream >> file_number)) {
        return Status::Corruption("invalid MANIFEST delete_file entry");
      }
      auto& files = edit->sstables;
      files.erase(std::remove_if(files.begin(), files.end(),
                                 [file_number](const SSTableMeta& file) {
                                   return file.file_number == file_number;
                                 }),
                  files.end());
    } else if (tag == "add_file") {
      SSTableMeta meta;
      std::string file_name;
      if (!(line_stream >> meta.file_number >> file_name)) {
        return Status::Corruption("invalid MANIFEST add_file entry");
      }
      meta.file_path = db_path / file_name;
      if (!(line_stream >> meta.level >> std::quoted(meta.smallest_key) >>
            std::quoted(meta.largest_key) >> meta.file_size)) {
        meta.level = 0;
        meta.smallest_key.clear();
        meta.largest_key.clear();
        meta.file_size = 0;
      }
      std::uint64_t parsed_number = 0;
      if (!ParseSSTableFileName(meta.file_path, &parsed_number) ||
          parsed_number != meta.file_number) {
        return Status::Corruption("invalid MANIFEST add_file name");
      }
      edit->sstables.erase(std::remove_if(edit->sstables.begin(), edit->sstables.end(),
                                          [&](const SSTableMeta& file) {
                                            return file.file_number == meta.file_number;
                                          }),
                           edit->sstables.end());
      edit->sstables.push_back(std::move(meta));
    } else {
      return Status::Corruption("unknown MANIFEST edit tag: " + tag);
    }
  }
  if (!saw_version) {
    return Status::Corruption("MANIFEST edit missing version");
  }
  std::sort(edit->sstables.begin(), edit->sstables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  return Status::OK();
}

Status ParseLegacyFullManifest(const std::filesystem::path& db_path,
                               const std::string& contents,
                               VersionEdit* loaded) {
  const size_t checksum_pos = contents.rfind("checksum ");
  if (checksum_pos == std::string::npos) {
    return Status::Corruption("MANIFEST missing checksum");
  }

  const std::string body = contents.substr(0, checksum_pos);
  std::istringstream checksum_stream(contents.substr(checksum_pos));
  std::string checksum_tag;
  std::uint32_t expected_checksum = 0;
  if (!(checksum_stream >> checksum_tag >> expected_checksum) || checksum_tag != "checksum") {
    return Status::Corruption("invalid MANIFEST checksum line");
  }
  std::string trailing;
  if (checksum_stream >> trailing) {
    return Status::Corruption("trailing MANIFEST checksum data");
  }
  if (CRC32(body) != expected_checksum) {
    return Status::Corruption("MANIFEST checksum mismatch");
  }

  bool saw_version = false;
  bool saw_next = false;
  bool saw_wal = false;
  bool saw_last_sequence = false;

  std::istringstream parsed(body);
  std::string tag;
  while (parsed >> tag) {
    if (tag == "version") {
      int version = 0;
      if (!(parsed >> version) || version != kManifestVersion) {
        return Status::Corruption("invalid MANIFEST version");
      }
      saw_version = true;
    } else if (tag == "next_file_number") {
      if (!(parsed >> loaded->next_file_number)) {
        return Status::Corruption("invalid MANIFEST next_file_number");
      }
      saw_next = true;
    } else if (tag == "wal_file_number") {
      if (!(parsed >> loaded->wal_file_number)) {
        return Status::Corruption("invalid MANIFEST wal_file_number");
      }
      saw_wal = true;
    } else if (tag == "last_sequence") {
      if (!(parsed >> loaded->last_sequence)) {
        return Status::Corruption("invalid MANIFEST last_sequence");
      }
      saw_last_sequence = true;
    } else if (tag == "sstable") {
      SSTableMeta meta;
      std::string file_name;
      if (!(parsed >> meta.file_number >> file_name)) {
        return Status::Corruption("invalid MANIFEST sstable entry");
      }
      meta.file_path = db_path / file_name;
      std::uint64_t parsed_number = 0;
      if (!ParseSSTableFileName(meta.file_path, &parsed_number) ||
          parsed_number != meta.file_number) {
        return Status::Corruption("invalid MANIFEST sstable file name");
      }
      if (!std::filesystem::exists(meta.file_path)) {
        return Status::IOError("MANIFEST references missing SSTable: " +
                               meta.file_path.string());
      }
      meta.level = 0;
      meta.smallest_key.clear();
      meta.largest_key.clear();
      std::error_code ec;
      meta.file_size = std::filesystem::file_size(meta.file_path, ec);
      if (ec) {
        meta.file_size = 0;
      }
      loaded->sstables.push_back(std::move(meta));
    } else {
      return Status::Corruption("unknown MANIFEST tag: " + tag);
    }
  }

  if (!parsed.eof()) {
    return Status::Corruption("failed to parse MANIFEST");
  }
  if (!saw_version || !saw_next || !saw_wal || !saw_last_sequence) {
    return Status::Corruption("MANIFEST missing required fields");
  }

  std::sort(loaded->sstables.begin(), loaded->sstables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  return Status::OK();
}

}  // namespace

Manifest::Manifest(std::filesystem::path db_path)
    : db_path_(std::move(db_path)), manifest_path_(db_path_ / kManifestFileName) {}

bool Manifest::Exists() const {
  return std::filesystem::exists(CurrentPath(db_path_)) ||
         std::filesystem::exists(manifest_path_);
}

Status Manifest::Load() {
  const std::filesystem::path current_path = CurrentPath(db_path_);
  if (std::filesystem::exists(current_path)) {
    std::ifstream current(current_path);
    if (!current.is_open()) {
      return Status::IOError("failed to open CURRENT: " + current_path.string());
    }
    std::string manifest_name;
    std::getline(current, manifest_name);
    std::string trailing;
    if (manifest_name.empty() || (current >> trailing)) {
      return Status::Corruption("invalid CURRENT file");
    }
    if (manifest_name.find('/') != std::string::npos ||
        manifest_name.find('\\') != std::string::npos) {
      return Status::Corruption("CURRENT must reference a local MANIFEST file");
    }
    manifest_path_ = db_path_ / manifest_name;
    if (!std::filesystem::exists(manifest_path_)) {
      return Status::IOError("CURRENT references missing MANIFEST: " +
                             manifest_path_.string());
    }
    return LoadEditLog();
  }

  std::ifstream in(manifest_path_, std::ios::binary);
  if (!in.is_open()) {
    return Status::IOError("failed to open MANIFEST: " + manifest_path_.string());
  }
  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (contents.rfind("record ", 0) == 0) {
    return LoadEditLog();
  }
  return LoadLegacyFullManifest(contents);
}

Status Manifest::LoadLegacyFullManifest(const std::string& contents) {
  VersionEdit loaded;
  Status status = ParseLegacyFullManifest(db_path_, contents, &loaded);
  if (!status.ok()) {
    return status;
  }
  edit_ = loaded;
  persisted_edit_ = loaded;
  legacy_format_loaded_ = true;
  return Status::OK();
}

Status Manifest::LoadEditLog() {
  std::ifstream in(manifest_path_, std::ios::binary);
  if (!in.is_open()) {
    return Status::IOError("failed to open MANIFEST: " + manifest_path_.string());
  }

  VersionEdit replayed;
  bool saw_record = false;
  while (true) {
    std::string header;
    if (!std::getline(in, header)) {
      if (in.eof()) {
        break;
      }
      return Status::IOError("failed to read MANIFEST edit header");
    }
    if (header.empty()) {
      return Status::Corruption("empty MANIFEST edit header");
    }

    std::istringstream parsed_header(header);
    std::string record_tag;
    size_t payload_size = 0;
    std::uint32_t expected_checksum = 0;
    if (!(parsed_header >> record_tag >> payload_size >> expected_checksum) ||
        record_tag != "record") {
      return Status::Corruption("invalid MANIFEST edit header");
    }
    std::string trailing;
    if (parsed_header >> trailing) {
      return Status::Corruption("trailing MANIFEST edit header data");
    }

    std::string payload(payload_size, '\0');
    in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (in.gcount() != static_cast<std::streamsize>(payload.size())) {
      break;
    }
    if (CRC32(payload) != expected_checksum) {
      return Status::Corruption("MANIFEST edit checksum mismatch");
    }

    Status status = ParseEditPayload(db_path_, payload, &replayed);
    if (!status.ok()) {
      return status;
    }
    saw_record = true;
  }

  if (!saw_record) {
    return Status::Corruption("MANIFEST edit log is empty");
  }
  for (const auto& file : replayed.sstables) {
    if (!std::filesystem::exists(file.file_path)) {
      return Status::IOError("MANIFEST references missing SSTable: " +
                             file.file_path.string());
    }
  }
  edit_ = replayed;
  persisted_edit_ = replayed;
  legacy_format_loaded_ = false;
  return Status::OK();
}

Status Manifest::WriteCurrentFile() {
  const std::filesystem::path current_path = CurrentPath(db_path_);
  const std::filesystem::path tmp_path = current_path.string() + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) {
      return Status::IOError("failed to open CURRENT temp file: " + tmp_path.string());
    }
    out << manifest_path_.filename().string() << '\n';
    out.flush();
    if (!out) {
      return Status::IOError("failed to write CURRENT temp file");
    }
  }

  Status status = FsyncFile(tmp_path);
  if (!status.ok()) {
    std::filesystem::remove(tmp_path);
    return status;
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, current_path, ec);
  if (ec) {
    std::filesystem::remove(tmp_path);
    return Status::IOError("failed to rename CURRENT temp file: " + ec.message());
  }
  return FsyncDirectory(db_path_);
}

Status Manifest::Save(const VersionEdit& edit) {
  std::filesystem::create_directories(db_path_);
  if (!std::filesystem::exists(CurrentPath(db_path_)) || legacy_format_loaded_) {
    const std::ios::openmode mode =
        legacy_format_loaded_ ? std::ios::trunc : std::ios::app;
    std::ofstream initialize(manifest_path_, mode);
    if (!initialize.is_open()) {
      return Status::IOError("failed to initialize MANIFEST: " + manifest_path_.string());
    }
    initialize.close();
    if (legacy_format_loaded_) {
      persisted_edit_ = VersionEdit{};
      legacy_format_loaded_ = false;
    }
    Status status = WriteCurrentFile();
    if (!status.ok()) {
      return status;
    }
  }

  const std::string payload = BuildEditPayload(persisted_edit_, edit);
  std::ostringstream header;
  header << "record " << payload.size() << ' ' << CRC32(payload) << '\n';

  {
    std::ofstream out(manifest_path_, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
      return Status::IOError("failed to open MANIFEST for append: " +
                             manifest_path_.string());
    }
    out << header.str();
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out) {
      return Status::IOError("failed to append MANIFEST edit");
    }
  }

  Status status = FsyncFile(manifest_path_);
  if (!status.ok()) {
    return status;
  }
  status = FsyncDirectory(db_path_);
  if (!status.ok()) {
    return status;
  }

  edit_ = edit;
  persisted_edit_ = edit;
  return Status::OK();
}

const std::vector<SSTableMeta>& Manifest::SSTables() const {
  return edit_.sstables;
}

std::uint64_t Manifest::NextFileNumber() const {
  return edit_.next_file_number;
}

std::uint64_t Manifest::WALFileNumber() const {
  return edit_.wal_file_number;
}

SequenceNumber Manifest::LastSequence() const {
  return edit_.last_sequence;
}

std::uint64_t Manifest::AllocateFileNumber() {
  return edit_.next_file_number++;
}

void Manifest::SetNextFileNumber(std::uint64_t number) {
  edit_.next_file_number = number;
}

void Manifest::SetWALFileNumber(std::uint64_t number) {
  edit_.wal_file_number = number;
}

void Manifest::SetLastSequence(SequenceNumber sequence) {
  edit_.last_sequence = sequence;
}

void Manifest::SetSSTables(std::vector<SSTableMeta> sstables) {
  std::sort(sstables.begin(), sstables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  edit_.sstables = std::move(sstables);
}

void Manifest::SetCurrentEdit(VersionEdit edit) {
  std::sort(edit.sstables.begin(), edit.sstables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  edit_ = std::move(edit);
}

VersionEdit Manifest::CurrentEdit() const {
  return edit_;
}

}  // namespace kv
