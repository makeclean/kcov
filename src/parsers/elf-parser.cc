#include <file-parser.hh>
#include <engine.hh>
#include <utils.hh>
#include <capabilities.hh>
#include <gcov.hh>
#include <phdr_data.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <libelf.h>
#include <dwarf.h>
#include <elfutils/libdw.h>
#include <map>
#include <vector>
#include <string>
#include <configuration.hh>
#include <database.hh>

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <link.h>

#include <filter.hh>
#include <libgen.h>

#include "address-verifier.hh"
#include "dwarf.hh"

using namespace kcov;

enum SymbolType
{
	SYM_NORMAL = 0,
	SYM_DYNAMIC = 1,
};

/**
 * Holder class for address segments
 */
class Segment
{
public:
	Segment(const void *data, uint64_t paddr, uint64_t vaddr, uint64_t size) :
		m_data(NULL), m_paddr(paddr), m_vaddr(vaddr), m_size(size)
{

		if (data) {
			m_data = xmalloc(size);
			memcpy(m_data, data, size);
		}
}

	Segment(const Segment &other) :
		m_data(NULL), m_paddr(other.m_paddr), m_vaddr(other.m_vaddr), m_size(other.m_size)
	{
		if (other.m_data) {
			m_data = xmalloc(other.m_size);
			memcpy(m_data, other.m_data, other.m_size);
		}
	}

	~Segment()
	{
		free(m_data);
	}

	/**
	 * Check if an address is contained within this segment
	 *
	 * @param addr the address to check
	 *
	 * @return true if valid
	 */
	bool addressIsWithinSegment(uint64_t addr) const
	{
		return addr >= m_paddr && addr < m_paddr + m_size;
	}

	/**
	 * Adjust an address with the segment.
	 *
	 * @param addr the address to adjust
	 *
	 * @return the new address
	 */
	uint64_t adjustAddress(uint64_t addr) const
	{
		if (addressIsWithinSegment(addr))
			return addr - m_paddr + m_vaddr;

		return addr;
	}

	uint64_t getBase() const
	{
		return m_vaddr;
	}

	const void *getData() const
	{
		return m_data;
	}

	size_t getSize() const
	{
		return m_size;
	}

private:
	void *m_data;

	// Should really be const, but GCC 4.6 doesn't like that
	uint64_t m_paddr;
	uint64_t m_vaddr;
	size_t m_size;
};
typedef std::vector<Segment> SegmentList_t;

class ElfInstance : public IFileParser, IFileParser::ILineListener
{
public:
	ElfInstance()
	{
		m_elf = NULL;
		m_addressVerifier = IAddressVerifier::create();
		m_filename = "";
		m_checksum = 0;
		m_elfIs32Bit = true;
		m_elfIsShared = false;
		m_isMainFile = true;
		m_initialized = false;
		m_filter = NULL;
		m_verifyAddresses = false;
		m_debuglinkCrc = 0;
		m_relocation = 0;
		m_invalidBreakpoints = 0;
		m_currentChecksum = 0;

		IParserManager::getInstance().registerParser(*this);
	}

	virtual ~ElfInstance()
	{
		delete m_addressVerifier;
	}

	uint64_t getChecksum()
	{
		return m_checksum;
	}

	std::string getParserType()
	{
		return "ELF";
	}

	bool elfIs64Bit()
	{
		return !m_elfIs32Bit;
	}

	void setupParser(IFilter *filter)
	{
		m_filter = filter;
	}

	enum IFileParser::PossibleHits maxPossibleHits()
	{
		return IFileParser::HITS_LIMITED; // Breakpoints are cleared after a hit
	}

	unsigned int matchParser(const std::string &filename, uint8_t *data, size_t dataSize)
	{
		Elf32_Ehdr *hdr = (Elf32_Ehdr *)data;

		if (memcmp(hdr->e_ident, ELFMAG, strlen(ELFMAG)) == 0)
			return match_perfect;

		return match_none;
	}

	bool addFile(const std::string &filename, struct phdr_data_entry *data)
	{
		if (!m_initialized) {
			m_verifyAddresses = IConfiguration::getInstance().keyAsInt("verify");

			panic_if(elf_version(EV_CURRENT) == EV_NONE,
					"ELF version failed\n");
			m_initialized = true;
		}


		m_filename = filename;

		m_buildId.clear();
		m_debuglink.clear();

		m_curSegments.clear();
		m_executableSegments.clear();
		for (uint32_t i = 0; data && i < data->n_segments; i++) {
			struct phdr_data_segment *seg = &data->segments[i];

			m_curSegments.push_back(Segment(NULL, seg->paddr, seg->vaddr, seg->size));
		}

		if (!checkFile())
			return false;


		// Has debug symbols? Otherwise, only collect is possible
		if (!DwarfParser().open(filename) && m_isMainFile)
			IConfiguration::getInstance().setKey("running-mode", IConfiguration::MODE_COLLECT_ONLY);

		for (FileListenerList_t::const_iterator it = m_fileListeners.begin();
				it != m_fileListeners.end();
				++it)
			(*it)->onFile(File(m_filename, m_currentChecksum, m_isMainFile ? IFileParser::FLG_NONE : IFileParser::FLG_TYPE_SOLIB));

		return true;
	}

	bool checkFile()
	{
		struct Elf *elf;
		bool out = true;
		int fd;

		fd = ::open(m_filename.c_str(), O_RDONLY, 0);
		if (fd < 0) {
				kcov_debug(ELF_MSG, "Cannot open %s\n", m_filename.c_str());
				return false;
		}

		if (!(elf = elf_begin(fd, ELF_C_READ, NULL)) ) {
				error("elf_begin failed on %s\n", m_filename.c_str());
				out = false;
				goto out_open;
		}
		if (elf_kind(elf) == ELF_K_NONE)
			out = false;

		if (m_isMainFile) {
			char *raw;
			size_t sz;
			uint16_t e_type;

			raw = elf_getident(elf, &sz);

			if (raw && sz > EI_CLASS) {
				ICapabilities &capabilities = ICapabilities::getInstance();

				m_elfIs32Bit = raw[EI_CLASS] == ELFCLASS32;

				if (elfIs64Bit() != machine_is_64bit())
					capabilities.removeCapability("handle-solibs");
				else
					capabilities.addCapability("handle-solibs");
			}

			e_type = m_elfIs32Bit ? elf32_getehdr(elf)->e_type : elf64_getehdr(elf)->e_type;

			m_elfIsShared = e_type == ET_DYN;
		}

		m_currentChecksum = m_elfIs32Bit ? elf32_checksum(elf) : elf64_checksum(elf);
		if (!m_checksum)
		{
			m_checksum = m_currentChecksum;
		}

		elf_end(elf);

out_open:
		close(fd);

		return out;
	}

	bool parse()
	{
		bool out = true;

		// should defer until setMainFileRelocation
		if (m_isMainFile && m_elfIsShared) {

			// ... but this needs to be done if we don't have solibs
			if (!IConfiguration::getInstance().keyAsInt("parse-solibs"))
				setMainFileRelocation(0);
		} else {
			out = doParse(0);
		}

		m_isMainFile = false;

		return out;
	}

	bool doParse(unsigned long relocation)
	{
		struct stat st;

		if (lstat(m_filename.c_str(), &st) < 0)
			return false;

		parseOneElf();

		// Gcov data?
		if (IConfiguration::getInstance().keyAsInt("gcov") && !m_gcnoFiles.empty())
			parseGcnoFiles(relocation);
		else
			parseOneDwarf(relocation);

		return true;
	}

	bool setMainFileRelocation(unsigned long relocation)
	{
		kcov_debug(INFO_MSG, "main file relocation = %#lx\n", relocation);

		if (m_elfIsShared) {
			if (!doParse(relocation))
				return false;
		} else {
			// this situation is probably problematic, as we have already notified
			// segment informations to the listeners.
			if (relocation != 0) {
				warning("Got a static executable with relocation=%#lx, "
					"probably the trace wouldn't work.", relocation);
			}
		}

		return true;
	}

	void parseGcnoFiles(unsigned long relocation)
	{
		for (FileList_t::const_iterator it = m_gcnoFiles.begin();
				it != m_gcnoFiles.end();
				++it) {
			const std::string &cur = *it;

			parseOneGcno(cur, relocation);
		}
	}

	void parseOneGcno(const std::string &filename, unsigned long relocation)
	{
		size_t sz;
		void *data;

		// The data is freed by the parser
		data = read_file(&sz, "%s", filename.c_str());
		if (!data)
			return;

		// Parse this gcno file!
		GcnoParser parser((uint8_t *)data, sz);

		// Parsing error?
		if (!parser.parse()) {
			warning("Can't parse %s\n", filename.c_str());

			return;
		}

		const GcnoParser::BasicBlockList_t &bbs = parser.getBasicBlocks();

		for (GcnoParser::BasicBlockList_t::const_iterator it = bbs.begin();
				it != bbs.end();
				++it) {
			const GcnoParser::BasicBlockMapping &cur = *it;

			// Report a generated address
			for (LineListenerList_t::const_iterator it = m_lineListeners.begin();
					it != m_lineListeners.end();
					++it)
				(*it)->onLine(cur.m_file, cur.m_line,
						gcovGetAddress(cur.m_file, cur.m_function, cur.m_basicBlock, cur.m_index) + relocation);
		}
	}

	bool parseOneDwarf(unsigned long relocation)
	{
		unsigned invalidBreakpoints = 0;

		m_invalidBreakpoints = 0;
		m_relocation = relocation;

		DwarfParser dp;

		bool rv = dp.open(m_filename);

		if (!rv && m_buildId.length() > 0) {
			/* Look for separate debug info: build-ids */
			std::string debug_file = std::string("/usr/lib/debug/.build-id/" +
							     m_buildId.substr(0,2) +
							     "/" +
							     m_buildId.substr(2, std::string::npos) +
							     ".debug");

			rv = dp.open(debug_file);
			if (!rv && m_isMainFile)
				kcov_debug(ELF_MSG, "Cannot open %s\n", debug_file.c_str());
		}

		if (!rv && m_debuglink.length() > 0) {
			/* Look for separate debug info: debug-links */
			std::string debugPath = lookupDebuglinkFile();

			if (debugPath == "" && m_isMainFile)
				kcov_debug(ELF_MSG, "Cannot open debug-link file in standard locations\n");
			else
				rv = dp.open(debugPath);
		}

		if (!rv) {
			IDatabaseReader &reader = IDatabaseReader::getInstance();

			const std::vector<uint64_t> &addrs = reader.get(m_currentChecksum);

			kcov_debug(ELF_MSG, "No debug symbols in %s.\n", m_filename.c_str());

			if (addrs.empty() && m_isMainFile) {
				warning("kcov requires binaries built with -g/-ggdb, a build-id file\n"
						"or GNU debug link information.\n");

				return false;
			}

			// Report all addresses (without file/line)
			for (std::vector<uint64_t>::const_iterator it = addrs.begin();
					it != addrs.end();
					++it)
				onLine("", 1, *it);

			return true;
		}

		/* Iterate over the headers */
		dp.forEachLine(*this);

		if (m_invalidBreakpoints > 0) {
			kcov_debug(STATUS_MSG, "kcov: %u invalid breakpoints skipped in %s\n",
					invalidBreakpoints, m_filename.c_str());
		}

		return true;
	}

	bool parseOneElf()
	{
		Elf_Scn *scn = NULL;
		size_t shstrndx;
		bool ret = false;
		bool setupSegments = false;
		FileList_t gcdaFiles; // List of gcov data files scanned from .rodata
		bool doScanForGcda = IConfiguration::getInstance().keyAsInt("gcov");
		char *fileData;
		size_t fileSize;
		unsigned int i;

		fileData = (char *)read_file(&fileSize, "%s", m_filename.c_str());
		if (!fileData) {
				error("Cannot open %s\n", m_filename.c_str());
				return false;
		}

		if (!(m_elf = elf_memory(fileData, fileSize)) ) {
				error("elf_begin failed on %s\n", m_filename.c_str());
				free(fileData);
				return false;
		}

		m_addressVerifier->setup(fileData, EI_NIDENT);

		if (elf_getshdrstrndx(m_elf, &shstrndx) < 0) {
				error("elf_getshstrndx failed on %s\n", m_filename.c_str());
				goto out_elf_begin;
		}

		setupSegments = m_curSegments.size() == 0;
		while ( (scn = elf_nextscn(m_elf, scn)) != NULL )
		{
			uint64_t sh_type;
			uint64_t sh_addr;
			uint64_t sh_size;
			uint64_t sh_flags;
			uint64_t sh_name;
			uint64_t sh_offset;
			uint64_t n_namesz;
			uint64_t n_descsz;
			uint64_t n_type;
			char *n_data;
			char *name;

			if (m_elfIs32Bit) {
				Elf32_Shdr *shdr32 = elf32_getshdr(scn);

				sh_type = shdr32->sh_type;
				sh_addr = shdr32->sh_addr;
				sh_size = shdr32->sh_size;
				sh_flags = shdr32->sh_flags;
				sh_name = shdr32->sh_name;
				sh_offset = shdr32->sh_offset;
			} else {
				Elf64_Shdr *shdr64 = elf64_getshdr(scn);

				sh_type = shdr64->sh_type;
				sh_addr = shdr64->sh_addr;
				sh_size = shdr64->sh_size;
				sh_flags = shdr64->sh_flags;
				sh_name = shdr64->sh_name;
				sh_offset = shdr64->sh_offset;
			}

			Elf_Data *data = elf_getdata(scn, NULL);

			name = elf_strptr(m_elf, shstrndx, sh_name);
			if(!data) {
					error("elf_getdata failed on section %s in %s\n",
							name, m_filename.c_str());
					goto out_elf_begin;
			}

			// Parse rodata to find gcda files
			if (doScanForGcda && data->d_buf && strcmp(name, ".rodata") == 0) {
				const char *dataPtr = (const char *)data->d_buf;

				for (size_t i = 0; i < data->d_size - 5; i++) {
					const char *p = &dataPtr[i];

					if (memcmp(p, (const void *)"gcda\0", 5) != 0)
						continue;

					const char *gcda = p;

					// Rewind to start of string
					while (gcda != dataPtr && *gcda != '\0')
						gcda--;

					// Rewound until start of rodata?
					if (gcda == dataPtr)
						continue;

					std::string file(gcda + 1);

					gcdaFiles.push_back(file);

					// Notify listeners that we have found gcda files
					for (FileListenerList_t::const_iterator it = m_fileListeners.begin();
							it != m_fileListeners.end();
							++it)
						(*it)->onFile(File(file, IFileParser::FLG_TYPE_COVERAGE_DATA));
				}
			}

			if (sh_type == SHT_NOTE && data->d_buf) {
				if (m_elfIs32Bit) {
					Elf32_Nhdr *nhdr32 = (Elf32_Nhdr *)data->d_buf;

					n_namesz = nhdr32->n_namesz;
					n_descsz = nhdr32->n_descsz;
					n_type = nhdr32->n_type;
					n_data = (char *)data->d_buf + sizeof (Elf32_Nhdr);
				} else {
					Elf64_Nhdr *nhdr64 = (Elf64_Nhdr *)data->d_buf;

					n_namesz = nhdr64->n_namesz;
					n_descsz = nhdr64->n_descsz;
					n_type = nhdr64->n_type;
					n_data = (char *)data->d_buf + sizeof (Elf64_Nhdr);
				}

				if (::strcmp(n_data, ELF_NOTE_GNU) == 0 &&
				    n_type == NT_GNU_BUILD_ID) {
					const char *hex_digits = "0123456789abcdef";
					unsigned char *build_id;

					build_id = (unsigned char *)n_data + n_namesz;
					for (i = 0; i < n_descsz; i++) {
						m_buildId.push_back(hex_digits[(build_id[i] >> 4) & 0xf]);
						m_buildId.push_back(hex_digits[(build_id[i] >> 0) & 0xf]);
					}
				}
			}

			// Check for debug links
			if (strcmp(name, ".gnu_debuglink") == 0) {
				const char *p = (const char *)data->d_buf;
				m_debuglink.append(p);
				const char *endOfString = p + strlen(p) + 1;

				// Align address for the CRC32
				unsigned long addr = (unsigned long)(endOfString - p);
				unsigned long offs = 0;

				if ((addr & 3) != 0)
					offs = 4 - (addr & 3);
				// ... and read out the CRC32
				memcpy((void *)&m_debuglinkCrc, endOfString + offs, sizeof(m_debuglinkCrc));
			}

			if ((sh_flags & (SHF_EXECINSTR | SHF_ALLOC)) != (SHF_EXECINSTR | SHF_ALLOC))
				continue;

			Segment seg(fileData + sh_offset, sh_addr, sh_addr, sh_size);
			// If we have segments already, we can safely skip this
			if (setupSegments)
				m_curSegments.push_back(seg);
			m_executableSegments.push_back(seg);
		}

		// If we have gcda files, try to find the corresponding gcno dittos
		for (FileList_t::iterator it = gcdaFiles.begin();
				it != gcdaFiles.end();
				++it) {
			std::string &gcno = *it; // Modify in-place
			size_t sz = gcno.size();

			// .gcda -> .gcno
			gcno[sz - 2] = 'n';
			gcno[sz - 1] = 'o';

			if (file_exists(gcno))
				m_gcnoFiles.push_back(gcno);
		}
		free(fileData);

		ret = true;

out_elf_begin:
		elf_end(m_elf);

		return ret;
	}

	void registerLineListener(IFileParser::ILineListener &listener)
	{
		m_lineListeners.push_back(&listener);
	}

	void registerFileListener(IFileParser::IFileListener &listener)
	{
		m_fileListeners.push_back(&listener);
	}

private:
	typedef std::vector<IFileParser::ILineListener *> LineListenerList_t;
	typedef std::vector<IFileListener *> FileListenerList_t;
	typedef std::vector<std::string> FileList_t;

	bool addressIsValid(uint64_t addr, unsigned &invalidBreakpoints) const
	{
		for (SegmentList_t::const_iterator it = m_executableSegments.begin();
				it != m_executableSegments.end();
				++it) {
			if (it->addressIsWithinSegment(addr)) {
				bool out = true;

				if (m_verifyAddresses) {
					uint64_t offset = addr - it->getBase();

					out = m_addressVerifier->verify(it->getData(),it->getSize(), offset);

					if (!out) {
						kcov_debug(ELF_MSG, "kcov: Address 0x%llx is not at an instruction boundary, skipping\n",
								(unsigned long long)addr);
						invalidBreakpoints++;
					}
				}

				return out;
			}
		}

		return false;
	}

	uint64_t adjustAddressBySegment(uint64_t addr)
	{
		for (SegmentList_t::const_iterator it = m_curSegments.begin();
				it != m_curSegments.end();
				++it) {
			if (it->addressIsWithinSegment(addr)) {
				addr = it->adjustAddress(addr);
				break;
			}
		}

		return addr;
	}


	// From IFileParser::ILineListener
	void onLine(const std::string &file, unsigned int lineNr, uint64_t addr)
	{
		if (!addressIsValid(addr, m_invalidBreakpoints))
			return;

		std::string rp = m_filter->mangleSourcePath(file);

		for (LineListenerList_t::const_iterator it = m_lineListeners.begin();
				it != m_lineListeners.end();
				++it)
			(*it)->onLine(rp, lineNr, adjustAddressBySegment(addr) + m_relocation);
	}


	std::string tryDebugLink(const std::string &path)
	{
		if (!file_exists(path))
			return "";

		size_t sz;
		uint8_t *p = (uint8_t *)read_file(&sz, "%s", path.c_str());
		if (!p)
			return "";
		uint32_t crc = debugLinkCrc32(0, p, sz);
		free(p);

		if (crc != m_debuglinkCrc) {
			kcov_debug(ELF_MSG, "CRC mismatch for debug link %s. Should be 0x%08x, is 0x%08x!\n",
					path.c_str(), m_debuglinkCrc, crc);
			return "";
		}

		return path;
	}

	std::string lookupDebuglinkFile()
	{
		std::string filePath;
		std::string debugPath;
		char *cpy;

		cpy = ::strdup(m_filename.c_str());
		filePath = std::string(::dirname(cpy));
		free(cpy);

		// Use debug link from the ELF (same directory as binary)
		debugPath = tryDebugLink(fmt("%s/%s", filePath.c_str(), m_debuglink.c_str()));
		if (debugPath != "")
			return debugPath;

		// Same directory .debug
		debugPath = tryDebugLink(fmt("%s/.debug/%s", filePath.c_str(), m_debuglink.c_str()));
		if (debugPath != "")
			return debugPath;

		return tryDebugLink(fmt("/usr/lib/debug/%s/%s", get_real_path(filePath).c_str(), m_debuglink.c_str()));
	}

	// From https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html
	uint32_t debugLinkCrc32 (uint32_t crc, unsigned char *buf, size_t len)
	{
		static const uint32_t crc32_table[256] =
		{
				0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419,
				0x706af48f, 0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4,
				0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07,
				0x90bf1d91, 0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
				0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856,
				0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
				0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4,
				0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
				0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3,
				0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac, 0x51de003a,
				0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599,
				0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
				0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190,
				0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f,
				0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e,
				0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
				0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed,
				0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
				0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3,
				0xfbd44c65, 0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
				0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a,
				0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5,
				0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa, 0xbe0b1010,
				0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
				0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17,
				0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6,
				0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615,
				0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
				0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 0xf00f9344,
				0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
				0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a,
				0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
				0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1,
				0xa6bc5767, 0x3fb506dd, 0x48b2364b, 0xd80d2bda, 0xaf0a1b4c,
				0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef,
				0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
				0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe,
				0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31,
				0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c,
				0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
				0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b,
				0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
				0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1,
				0x18b74777, 0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
				0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45, 0xa00ae278,
				0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7,
				0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66,
				0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
				0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605,
				0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8,
				0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b,
				0x2d02ef8d
		};
		unsigned char *end;

		crc = ~crc & 0xffffffff;
		for (end = buf + len; buf < end; ++buf)
			crc = crc32_table[(crc ^ *buf) & 0xff] ^ (crc >> 8);

		return ~crc & 0xffffffff;
	}

	SegmentList_t m_curSegments;
	SegmentList_t m_executableSegments;
	FileList_t m_gcnoFiles;

	IAddressVerifier *m_addressVerifier;
	bool m_verifyAddresses;
	struct Elf *m_elf;
	bool m_elfIs32Bit;
	bool m_elfIsShared;
	LineListenerList_t m_lineListeners;
	FileListenerList_t m_fileListeners;
	std::string m_filename;
	std::string m_buildId;
	std::string m_debuglink;
	uint32_t m_debuglinkCrc;
	bool m_isMainFile;
	uint64_t m_checksum;
	uint64_t m_currentChecksum;
	bool m_initialized;
	uint64_t m_relocation;
	uint32_t m_invalidBreakpoints;

	/***** Add strings to update path information. *******/
	std::string m_origRoot;
	std::string m_newRoot;

	IFilter *m_filter;
};

static ElfInstance g_instance;
