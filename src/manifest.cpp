#include "manifest.h"

#include <algorithm>
#include <charconv>
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

bool ReadUnsigned(std::istream& in, std::uint64_t* value) {
  std::string token;
  if (!(in >> token) || token.empty()) return false;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), *value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

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
          << item.second.file_size;

      out << '\n';
    }
  }
  return out.str();
}

Status PersistEditRecord(const std::filesystem::path& path,
                         const VersionEdit& persisted,
                         const VersionEdit& edit,
                         std::ios::openmode mode,
                         const std::function<Status()>& before_sync) {
  const std::string payload = BuildEditPayload(persisted, edit);
  std::ostringstream header;
  header << "record " << payload.size() << ' ' << CRC32(payload) << '\n';

  {
    std::ofstream out(path, std::ios::binary | mode);
    if (!out.is_open()) {
      return Status::IOError("failed to open MANIFEST for write: " + path.string());
    }
    out << header.str();
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out) {
      return Status::IOError("failed to write MANIFEST edit");
    }
  }
  if (before_sync) {
    Status status = before_sync();
    if (!status.ok()) return status;
  }
  return FsyncFile(path);
}

Status TruncateTornManifest(const std::filesystem::path& path,
                            std::uintmax_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  if (ec) {
    return Status::IOError("failed to truncate torn MANIFEST: " + ec.message());
  }
  Status status = FsyncFile(path);
  if (!status.ok()) {
    return status;
  }
  return FsyncDirectory(path.parent_path());
}

Status ParseEditPayload(const std::filesystem::path& db_path,
                        const std::string& payload,
                        VersionEdit* edit) {
  std::istringstream parsed(payload);
  bool saw_version = false;
  bool saw_next = false, saw_wal = false, saw_sequence = false;
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
      if (!ReadUnsigned(line_stream, &edit->next_file_number)) {
        return Status::Corruption("invalid MANIFEST edit next_file_number");
      }
      saw_next = true;
    } else if (tag == "wal_file_number") {
      if (!ReadUnsigned(line_stream, &edit->wal_file_number)) {
        return Status::Corruption("invalid MANIFEST edit wal_file_number");
      }
      saw_wal = true;
    } else if (tag == "last_sequence") {
      if (!ReadUnsigned(line_stream, &edit->last_sequence)) {
        return Status::Corruption("invalid MANIFEST edit last_sequence");
      }
      saw_sequence = true;
    } else if (tag == "delete_file") {
      std::uint64_t file_number = 0;
      if (!ReadUnsigned(line_stream, &file_number)) {
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
      if (!ReadUnsigned(line_stream, &meta.file_number) || !(line_stream >> file_name)) {
        return Status::Corruption("invalid MANIFEST add_file entry");
      }
      meta.file_path = db_path / file_name;
      if (file_name.find('/') != std::string::npos || file_name.find('\\') != std::string::npos) {
        return Status::Corruption("MANIFEST SSTable must have a local file name");
      }
      if (!(line_stream >> meta.level >> std::quoted(meta.smallest_key) >>
            std::quoted(meta.largest_key)) || !ReadUnsigned(line_stream, &meta.file_size)) {
        return Status::Corruption("invalid MANIFEST file metadata");
      }
      std::uint64_t parsed_number = 0;
      if (meta.level < 0 || meta.level > 2 ||
          (!meta.smallest_key.empty() && !meta.largest_key.empty() &&
           meta.smallest_key > meta.largest_key)) {
        return Status::Corruption("invalid MANIFEST level or key range");
      }
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
    line_stream >> std::ws;
    if (!line_stream.eof()) return Status::Corruption("trailing MANIFEST edit data");
  }
  if (!saw_version || !saw_next || !saw_wal || !saw_sequence ||
      edit->next_file_number == 0 || edit->wal_file_number == 0) {
    return Status::Corruption("MANIFEST edit missing required fields or invalid file numbers");
  }
  std::sort(edit->sstables.begin(), edit->sstables.end(),
            [](const SSTableMeta& lhs, const SSTableMeta& rhs) {
              return lhs.file_number < rhs.file_number;
            });
  return Status::OK();
}

}  // namespace

Manifest::Manifest(std::filesystem::path db_path, std::function<Status()> before_sync)
    : db_path_(std::move(db_path)), manifest_path_(db_path_ / kManifestFileName),
      before_sync_(std::move(before_sync)) {}

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
  if (contents.rfind("record ", 0) != 0) {
    return Status::Corruption("MANIFEST is not a record edit log");
  }
  return LoadEditLog();
}

Status Manifest::LoadEditLog() {
  std::ifstream in(manifest_path_, std::ios::binary);
  if (!in.is_open()) {
    return Status::IOError("failed to open MANIFEST: " + manifest_path_.string());
  }

  VersionEdit replayed;
  bool saw_record = false;
  bool torn_tail = false;
  std::uintmax_t last_good_offset = 0;
  while (true) {
    const std::uintmax_t record_offset = last_good_offset;
    std::string header;
    if (!std::getline(in, header)) {
      if (in.eof()) {
        break;
      }
      return Status::IOError("failed to read MANIFEST edit header");
    }
    if (in.eof()) {
      torn_tail = true;
      break;
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

    const auto payload_position = in.tellg();
    in.seekg(0, std::ios::end);
    const auto end_position = in.tellg();
    if (payload_position < 0 || end_position < payload_position) {
      return Status::IOError("failed to determine MANIFEST size");
    }
    if (payload_size > static_cast<std::uintmax_t>(end_position - payload_position)) {
      torn_tail = true;
      break;
    }
    in.seekg(payload_position);
    std::string payload(payload_size, '\0');
    in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (in.gcount() != static_cast<std::streamsize>(payload.size())) {
      if (!in.eof()) {
        return Status::IOError("failed to read MANIFEST edit payload");
      }
      torn_tail = true;
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
    last_good_offset = record_offset + header.size() + 1 + payload.size();
  }

  if (!saw_record) {
    return Status::Corruption("MANIFEST edit log is empty");
  }
  if (torn_tail) {
    in.close();
    Status status = TruncateTornManifest(manifest_path_, last_good_offset);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& file : replayed.sstables) {
    if (!std::filesystem::exists(file.file_path)) {
      return Status::IOError("MANIFEST references missing SSTable: " +
                             file.file_path.string());
    }
  }
  edit_ = replayed;
  persisted_edit_ = replayed;
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
    (void)RemoveFileIfExists(tmp_path);
    return status;
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, current_path, ec);
  if (ec) {
    (void)RemoveFileIfExists(tmp_path);
    return Status::IOError("failed to rename CURRENT temp file: " + ec.message());
  }
  return FsyncDirectory(db_path_);
}

Status Manifest::Save(const VersionEdit& edit) {
  std::error_code directory_ec;
  std::filesystem::create_directories(db_path_, directory_ec);
  if (directory_ec) return Status::IOError("failed to create MANIFEST directory: " + directory_ec.message());
  const bool current_exists = std::filesystem::exists(CurrentPath(db_path_), directory_ec);
  if (directory_ec) return Status::IOError("failed to inspect CURRENT: " + directory_ec.message());

  Status status =
      PersistEditRecord(manifest_path_, persisted_edit_, edit, std::ios::app, before_sync_);
  if (!status.ok()) {
    return status;
  }
  status = FsyncDirectory(db_path_);
  if (!status.ok()) {
    return status;
  }
  if (!current_exists) {
    status = WriteCurrentFile();
    if (!status.ok()) {
      return status;
    }
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
