//TODO: "If PK_CAPS_HIDE is set, the plugin will not show the file type as a packer. This is useful for plugins which are mainly used for creating files, e.g. to create batch files, avi files etc. The file needs to be opened with Ctrl+PgDn in this case, because Enter will launch the associated application."
//    ==>altho this would require a second build with different filenames etc - the "gibberish extension"-solution is clumsy, but easier \o/
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <cstdlib>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEPARATOR "\\"
#else
#include <unistd.h>
#define PATH_SEPERATOR "/"
#define __stdcall
#define sprintf_s(a,b,...) sprintf(a,__VA_ARGS__)
#define strcpy_s(a,b,c) strcpy(a,c)
#define fopen_s(a,b,c) fopen(b,c)
#define _strcmpi strcasecmp
#define BOOL int
typedef char *LPCSTR;
#define _stat stat
#endif

#include "wcxhead.h"

#include "dosfs-1.03/dosfs.h"
#include "jacknife.h"

stEntryList entryList;

tArchive* pCurrentArchive;

typedef tArchive* myHANDLE;

DISK_IMAGE_INFO disk_image = { 0 };

//unpack MSA into a newly created buffer
uint8_t *unpack_msa(tArchive *arch, uint8_t *packedMsa, int packedSize) {
	int sectors = ((int)packedMsa[2] << 8) | ((int)packedMsa[3]);
	int sides = (((int)packedMsa[4] << 8) | ((int)packedMsa[5])) + 1;
	int startTrack = ((int)packedMsa[6] << 8) | ((int)packedMsa[7]);
	int endTrack = ((int)packedMsa[8] << 8) | ((int)packedMsa[9]);
	//just ignore partial disk images, skipping tracks would skip bpb/fat, too
	if (startTrack != 0 || endTrack == 0) {
		free(packedMsa);
		return NULL;
	}
	int unpackedSize = sectors * 512 * sides * (endTrack + 1);
	disk_image.image_sectors = sectors;
	disk_image.image_sides = sides;
	disk_image.image_tracks = endTrack;
	uint8_t *unpackedData = (uint8_t *)malloc(unpackedSize);
	if (!unpackedData) return 0;

	int offset = 10;
	int out = 0;
	for (int i = 0; i < (endTrack + 1) * sides; i++) {
		int trackLen = packedMsa[offset++];
		trackLen <<= 8;
		trackLen += packedMsa[offset++];
		if (trackLen != 512 * sectors) {
			for (; trackLen > 0; trackLen--) {
				unpackedData[out++] = packedMsa[offset++];
				// Bounds check against corrupt MSA images
				if (out > unpackedSize || offset > packedSize)
				{
					free(unpackedData);
					return 0;
				}
				if (unpackedData[out - 1] == 0xe5) {
					// Bounds check against corrupt MSA images
					if (offset + 4 - 1 > packedSize)
					{
						free(unpackedData);
						return 0;
					}
					uint8_t data = packedMsa[offset++];
					unsigned int runLen = packedMsa[offset++];
					runLen <<= 8;
					runLen += packedMsa[offset++];
					trackLen -= 3;
					out--;
					for (unsigned int ii = 0; ii < runLen && out < unpackedSize; ii++) {
						unpackedData[out++] = data;
					}
				}
			}
		}
		else {
			// Bounds check against corrupt MSA images
			if (out + trackLen > unpackedSize || offset + trackLen > packedSize)
			{
				free(unpackedData);
				return 0;
			}
			for (; trackLen > 0; trackLen--) {
				unpackedData[out++] = packedMsa[offset++];
			}
		}
	}
	disk_image.file_size = unpackedSize;
	return unpackedData;
}



bool guess_size(int size)
{
	if (size % 512) {
		return false;
	}
	int tracks, sectors;
	for (tracks = 86; tracks > 0; tracks--) {
		for (sectors = 11; sectors >= 9; sectors--) {
			if (!(size % tracks)) {
				if ((size % (tracks * sectors * 2 * 512)) == 0) {
					disk_image.image_tracks = tracks;
					disk_image.image_sides = 2;
					disk_image.image_sectors = sectors;
					return true;
				}
				else if ((size % (tracks * sectors * 1 * 512)) == 0) {
					disk_image.image_tracks = tracks;
					disk_image.image_sides = 1;
					disk_image.image_sectors = sectors;
					return true;
				}
			}
		}
	}
	return false;
}

#define BYTE_SWAP_WORD(a) ((unsigned short)(a>>8)|(unsigned short)(a<<8))

unsigned char *expand_dim(bool fastcopy_header)
{ 
	unsigned char *buf = (unsigned char *)malloc(disk_image.image_tracks * disk_image.image_sectors * disk_image.image_sides * 512);
	unsigned char *d = buf;
	if (!d) return 0;
	unsigned char *s = disk_image.buffer;

	FCOPY_HEADER *h = (FCOPY_HEADER * )s;
	s += 32;

	int total_filesystem_sectors = BYTE_SWAP_WORD(h->total_filesystem_sectors);
	int bytes_left = (int)(disk_image.file_size - total_filesystem_sectors * 512);
	if (fastcopy_header)
		bytes_left -= 32;

	memcpy(d, s, total_filesystem_sectors * 512);
	s += total_filesystem_sectors * 512;
	d += total_filesystem_sectors * 512;

	unsigned char *fat1 = disk_image.buffer + 512+32 + 3; // A bit hardcoded, but eh
	int cluster_size = BYTE_SWAP_WORD(h->cluster_size);

	for (int i = 0; i < BYTE_SWAP_WORD(h->total_clusters) / 2; i++)
	{
		// Check "even" entry in a FAT12 record
		int fat_entry = ((fat1[1] & 0xf) << 8) | fat1[0];
		if (fat_entry == 0 || (fat_entry >= 0xff0 && fat_entry <= 0xff7) ||bytes_left<=0)
		{
			memset(d, 0, 512);
			d += cluster_size;
		}
		else
		{
			if (bytes_left >= cluster_size)
			{
				memcpy(d, s, cluster_size);
			}
			else
			{
				// Yes, there are .dim files that are truncated at the end
				memcpy(d, s, bytes_left);
				memset(d + bytes_left, 0, cluster_size - bytes_left);
			}
			s += cluster_size;
			d += cluster_size;
			bytes_left -= cluster_size;
		}

		// Check "odd" entry in a FAT12 record
		fat_entry = (fat1[2] << 4) | (fat1[1] >> 4);
		if (fat_entry == 0 || (fat_entry >= 0xff0 && fat_entry <= 0xff7) || bytes_left <= 0)
		{
			memset(d, 0, 512);
			d += cluster_size;
		}
		else
		{
			if (bytes_left >= cluster_size)
			{
				memcpy(d, s, cluster_size);
			}
			else
			{
				// Yes, there are .dim files that are truncated at the end
				memcpy(d, s, bytes_left);
				memset(d + bytes_left, 0, cluster_size - bytes_left);
			}
			s += cluster_size;
			d += cluster_size;
			bytes_left -= cluster_size;
		}

		fat1 += 3;
	}

	return buf; 
}

/*
	Attach emulation to a host-side disk image file
	Returns 0 OK, nonzero for any error
*/
int DFS_HostAttach(tArchive *arch)
{
	disk_image.file_handle = fopen(arch->archname, "r+b");
	if (disk_image.file_handle == NULL)
		return -1;

	fseek(disk_image.file_handle, 0, SEEK_END);
	disk_image.file_size = _ftelli64(disk_image.file_handle);
	fseek(disk_image.file_handle, 0, SEEK_SET);

	disk_image.disk_geometry_does_not_match_bpb = false;
	disk_image.mode = DISKMODE_HARD_DISK;

	if (disk_image.file_size > 2880 * 1024)
	{
		// Hard disk image, that's all we need to do
		return 0;
	}

	// Definitely a disk image, let's cache it into RAM
	disk_image.buffer = (uint8_t *)malloc((size_t)disk_image.file_size);
	if (!disk_image.buffer) return -1;
	if (!fread(disk_image.buffer, (size_t)disk_image.file_size, 1, disk_image.file_handle)) { fclose(disk_image.file_handle); return -1; }
	fclose(disk_image.file_handle);
	disk_image.mode = DISKMODE_LINEAR;
	if ((disk_image.buffer[0] == 0xe && disk_image.buffer[1] == 0xf) ||
		(disk_image.buffer[0] == 0x0 && disk_image.buffer[1] == 0x0 && strlen(arch->archname) > 4 && _strcmpi(arch->archname + strlen(arch->archname) - 4, ".msa") == 0))
	{
		// MSA image, unpack it to a flat buffer
		disk_image.mode = DISKMODE_MSA;
		uint8_t *unpacked_msa = unpack_msa(arch, disk_image.buffer, (int)disk_image.file_size);
		free(disk_image.buffer);
		if (!unpacked_msa)
		{
			return -1;
		}
		disk_image.buffer = unpacked_msa;
	}
	else if (*(unsigned short *)disk_image.buffer == 0x4242)
	{
		// Fastcopy DIM image, unpack it to flat buffer
		FCOPY_HEADER *h = (FCOPY_HEADER *)disk_image.buffer;
		if (h->start_track)
		{
			// Nope, we don't support partial images
			return -1;
		}
		if (h->disk_configuration_present)
		{
			disk_image.image_sectors = h->sectors;
			disk_image.image_sides = h->sides + 1;
			disk_image.image_tracks = h->end_track + 1;
			if (h->get_sectors)
			{
				// Disk was imaged with "Get sectors" on, so we need to 
				// expand the image to fill in the non-imaged sectors with blanks
				disk_image.mode = DISKMODE_FCOPY_CONF_USED_SECTORS;
				uint8_t *expanded = expand_dim(true);
				if (!expanded)
				{
					return -1;
				}
				disk_image.buffer = expanded;
			}
			else
			{
				// No problem, just skip past the FCopy header and treat it as a normal .ST disk
				disk_image.mode = DISKMODE_FCOPY_CONF_ALL_SECTORS;
				disk_image.buffer += 32;
			}
		}
		else
		{
			disk_image.mode = DISKMODE_FCOPY_NO_CONF;
			disk_image.buffer += 32;
		}
	}
	else
	{
		if (!guess_size((int)disk_image.file_size))
		{
			free(disk_image.buffer);
			return -1;
		}
	}
	return DFS_OK;
}

uint32_t recalculate_sector(uint32_t sector)
{
	uint32_t requested_track = sector / disk_image.bpb_sectors_per_track / disk_image.bpb_sides;
	uint32_t requested_side = (sector % (disk_image.bpb_sectors_per_track * disk_image.bpb_sides)) / disk_image.bpb_sectors_per_track;
	uint32_t requested_sector = sector % disk_image.bpb_sectors_per_track;
	return requested_track * disk_image.image_sectors * disk_image.image_sides +
		requested_side * disk_image.image_sectors +
		requested_sector;
}

/*
	Read sector from image
	Returns 0 OK, nonzero for any error
*/
int DFS_HostReadSector(uint8_t *buffer, uint32_t sector, uint32_t count)
{
	if (disk_image.disk_geometry_does_not_match_bpb)
	{
		// Wonky disk image detected, let's skip the second side from the image
		sector = recalculate_sector(sector);
		assert(count == 1);	// Leave this here just to remind us that anything if count>1 it could mean Very Bad Things(tm)
	}

	// fseek into an opened for writing file can extend the file on Windows and won't fail, so let's check bounds
	if ((int)(sector * SECTOR_SIZE) > disk_image.file_size)
		return -1;

	if (disk_image.mode != DISKMODE_HARD_DISK)
	{
		memcpy(buffer, &disk_image.buffer[sector * SECTOR_SIZE], SECTOR_SIZE);
		return 0;
	}
	else
	{
		if (fseek(disk_image.file_handle, sector * SECTOR_SIZE, SEEK_SET))
			return -1;

		fread(buffer, SECTOR_SIZE, count, disk_image.file_handle);
		return 0;
	}
}

uint32_t DFS_ReadSector(uint8_t unit, uint8_t *buffer, uint32_t sector, uint32_t count)
{
	return DFS_HostReadSector(buffer, sector, count);
}

/*
	Write sector to image
	Returns 0 OK, nonzero for any error
*/
int DFS_HostWriteSector(uint8_t *buffer, uint32_t sector, uint32_t count)
{
	if (disk_image.disk_geometry_does_not_match_bpb)
	{
		// Wonky disk image detected, let's skip the second side from the image
		sector = recalculate_sector(sector);
		assert(count == 1);	// Leave this here just to remind us that anything if count>1 it could mean Very Bad Things(tm)
	}

	// fseek into an opened for writing file can extend the file on Windows and won't fail, so let's check bounds
	if ((int)(sector * SECTOR_SIZE) > disk_image.file_size)
		return -1;

	if (disk_image.mode != DISKMODE_HARD_DISK)
	{
		memcpy(&disk_image.buffer[sector * SECTOR_SIZE], buffer, SECTOR_SIZE);
		return 0;
	}
	else
	{
		if (fseek(disk_image.file_handle, sector * SECTOR_SIZE, SEEK_SET))
			return -1;

		fwrite(buffer, SECTOR_SIZE, count, disk_image.file_handle);
		fflush(disk_image.file_handle);
		return 0;
	}
}

uint32_t DFS_WriteSector(uint8_t unit, uint8_t *buffer, uint32_t sector, uint32_t count)
{
	return DFS_HostWriteSector(buffer, sector, count);
}

// try to pack a chunk of data in MSA RLE format
// returns packed size or -1 if packing was unsuccessful
static int pack_track(unsigned char *dest, const unsigned char *src, int len) {
	int pklen = 0;
	const unsigned char *p = (const unsigned char *)src, *src_end = (const unsigned char *)src + len;

	while (p < src_end) {
		const unsigned char *prev = p;
		unsigned int pkv = *p++;
		while (p < src_end && *p == pkv) p++;
		int n = (int)(p - prev);
		if ((n >= 4 || pkv == 0xE5) && pklen + 4 < len) {
			*dest++ = 0xE5;
			*dest++ = pkv;
			*dest++ = n >> 8;
			*dest++ = n;
			pklen += 4;
		}
		else if (pklen + n < len) {
			int i;
			for (i = 0; i < n; ++i) *dest++ = pkv;
			pklen += n;
		}
		else {
			return -1;
		}
	}
	return pklen;
}

uint8_t *make_msa(tArchive *arch)
{
	// Write MSA header
	int sectors = disk_image.image_sectors;
	int sides = disk_image.image_sides;
	int start_track = 0;
	int end_track = disk_image.image_tracks;

	unsigned char *packed_buffer = (unsigned char *)malloc(10 + end_track * (sectors * SECTOR_SIZE + 2) * sides + 100000); // 10=header size, +2 bytes per track for writing the track size
	if (!packed_buffer) return 0;
	unsigned char *pack = packed_buffer;

	memcpy(pack + 0, "\x0e\x0f", 2);
	*(unsigned short *)(pack + 2) = ((unsigned short)(sectors << 8)) | ((unsigned short)(sectors >> 8));
	*(unsigned short *)(pack + 4) = ((unsigned short)((sides - 1) << 8)) | ((unsigned short)((sides - 1) >> 8));
	*(unsigned short *)(pack + 6) = 0; // Start track will always be 0
	*(unsigned short *)(pack + 8) = ((unsigned short)(end_track << 8)) | ((unsigned short)(end_track >> 8));
	pack += 10;

	int track;
	unsigned char *p = disk_image.buffer;
	for (track = 0; track < end_track + 1; ++track) {
		int side;
		for (side = 0; side < sides; ++side) {
			// try to compress the track
			int pklen = pack_track(pack + 2, p, sectors * 512);
			if (pklen < 0) {
				// compression failed, writing uncompressed
				*(unsigned short *)(pack) = (unsigned short)((sectors * SECTOR_SIZE) >> 8) | (unsigned short)((sectors * SECTOR_SIZE) << 8);
				memcpy(pack + 2, p, sectors * 512);
				pack += 2 + SECTOR_SIZE * sectors;
			}
			else {
				// write the compressed data
				*(unsigned short *)(pack) = (unsigned short)(pklen >> 8) | (unsigned short)(pklen << 8);
				pack += 2 + pklen;
			}
			p += sectors * 512;
		}
	}
	disk_image.file_size = (int)(pack - packed_buffer);
	return packed_buffer;
}

int DFS_HostDetach(tArchive *arch)
{
	if (disk_image.mode != DISKMODE_HARD_DISK)
	{
		if (disk_image.mode == DISKMODE_FCOPY_CONF_ALL_SECTORS || disk_image.mode == DISKMODE_FCOPY_NO_CONF)
		{
			disk_image.buffer -= 32;
		}
		if (!arch->volume_dirty)
		{
			free(disk_image.buffer);
			return 0;
		}
		if (disk_image.mode == DISKMODE_MSA)
		{
			uint8_t *packed_msa = make_msa(arch);
			if (!packed_msa)
			{
				free(disk_image.buffer);
				return -1;
			}
			free(disk_image.buffer);
			disk_image.buffer = packed_msa;
		}
		if (disk_image.mode == DISKMODE_FCOPY_CONF_ALL_SECTORS || disk_image.mode == DISKMODE_FCOPY_CONF_USED_SECTORS)
		{
			// Eventually here we'll just add some code to write a .dim file using "all sectors",
			// unless someone reeeeeeeeally asks for "used sectors" format it's not happening
		}
		if (disk_image.mode == DISKMODE_FCOPY_NO_CONF)
		{
			// Unsure if the implementation is different here, it might be merged with the above block
		}
		disk_image.file_handle = fopen(arch->archname, "wb");
		if (!disk_image.file_handle) return -1;
		fwrite(disk_image.buffer, (size_t)disk_image.file_size, 1, disk_image.file_handle);
		free(disk_image.buffer);
		fclose(disk_image.file_handle);
		return 0;
	}
	else
	{
		if (!disk_image.file_handle) return -1;
		return fclose(disk_image.file_handle);
	}
}

stEntryList* findLastEntry() {
	stEntryList* entry = &entryList;
	while (entry->next != NULL) {
		entry = entry->next;
	}
	return entry;
}

void DirToCanonical(char dest[13], uint8_t *src)
{
	bool added_dot = false;
	for (int i = 0; i < 11; i++)
	{
		if (*src == ' ')
		{
			do
			{
				src++;
				i++;
				if (i == 11)
				{
					*dest = 0;
					return;
				}
			} while (*src == ' ');
			*dest++ = '.';
			added_dot = true;
		}
		*dest++ = *src++;
		if (i == 7 && !added_dot && *src!=' ') *dest++ = '.';
	}
	*dest = 0;
}

uint32_t scan_files(char* path, VOLINFO *vi, int partition)
{
	uint32_t res;
	int i;
	DIRINFO di;

	uint8_t *scratch_sector=(uint8_t *)malloc(SECTOR_SIZE);
	if (!scratch_sector) return DFS_ERRMISC;

	di.scratch = scratch_sector;
	stEntryList *lastEntry;

	// TODO: this can be supplied by the caller instead of calculated here (think: recursion)
	char partition_prefix[16] = { 0 };
	if (disk_image.mode == DISKMODE_HARD_DISK)
	{
		sprintf(partition_prefix, "%i" PATH_SEPARATOR, partition);
	}

	res = DFS_OpenDir(vi, (uint8_t *)path, &di);
	if (res == DFS_OK) {
		i = (int)strlen(path);
		for (;;) {
			lastEntry = findLastEntry();
			res = DFS_GetNext(vi, &di, &(*lastEntry).de);
			if (res != DFS_OK) break;
			if (lastEntry->de.name[0] == 0) continue;
			if (strcmp((char *)lastEntry->de.name, ".          \x10") == 0) continue;
			if (strcmp((char *)lastEntry->de.name, "..         \x10") == 0) continue;
			DirToCanonical(lastEntry->filename_canonical, lastEntry->de.name);
			if (lastEntry->de.attr & ATTR_VOLUME_ID) {
				strcpy((char *)vi->label, lastEntry->filename_canonical);
				continue;
			}
			if (lastEntry->de.attr & ATTR_DIRECTORY) {
				//if we exceed MAX_PATH this image has a serious problem, so better bail out
				if (strlen(path) + strlen(lastEntry->filename_canonical) + 1 >= MAX_PATH ||
					sprintf_s((char *)lastEntry->fileWPath, MAX_PATH, "%s%s%s", partition_prefix, path, lastEntry->filename_canonical) == -1) {
					res= DFS_ERRMISC;
					break;
				}
				if (i + strlen((char *)lastEntry->de.name) + 1 >= MAX_PATH ||
					sprintf_s(&path[i], MAX_PATH - i, "%s" PATH_SEPARATOR, lastEntry->filename_canonical) == -1) {
					res=DFS_ERRMISC;
					break;
				}
				lastEntry->next = new stEntryList();
				lastEntry->next->prev = lastEntry;
				lastEntry->next->next = NULL;
				lastEntry = lastEntry->next;
				res = scan_files(path, vi, partition);
				if (res != DFS_OK && res != DFS_EOF)
				{
					break;
				}
				path[i] = 0;
			}
			else {
				//if we exceed MAX_PATH this image has a serious problem, so better bail out
				if (strlen(path) + strlen(lastEntry->filename_canonical) + 1 >= MAX_PATH ||
					sprintf_s(lastEntry->fileWPath, MAX_PATH, "%s%s%s", partition_prefix, path, lastEntry->filename_canonical) == -1)
				{
					res = DFS_ERRMISC;
					break;
				}
				lastEntry->next = new stEntryList();
				lastEntry->next->prev = lastEntry;
				lastEntry->next->next = NULL;
				lastEntry = lastEntry->next;
			}
		}
	}

	free(scratch_sector);

	return res;
}

bool OpenImage(tOpenArchiveData *ArchiveData, tArchive *arch)
{
	ArchiveData->CmtBuf = 0;
	ArchiveData->CmtBufSize = 0;
	ArchiveData->CmtSize = 0;
	ArchiveData->CmtState = 0;
	ArchiveData->OpenResult = E_NO_MEMORY;// default error type

	if (DFS_HostAttach(arch) != DFS_OK)
	{
		ArchiveData->OpenResult = E_BAD_ARCHIVE;
		return false;
	}

	uint8_t scratch_sector[SECTOR_SIZE];

	// Obtain pointer to first partition on first (only) unit
	if (disk_image.mode == DISKMODE_HARD_DISK)
	{
		PART_INFO *p = disk_image.partition_info;
		VOLINFO *a = arch->vi;
		for (int i = 0; i < MAX_PARTITIONS; i++)
		{
			p->start_sector = DFS_GetPtnStart(0, scratch_sector, i, &p->active, (uint8_t *)p->type, &p->total_sectors);
			if (p->start_sector == 0xffffffff)
			{
				// Do nothing for now, as other partitions might be ok
				//printf("Cannot find first partition\n");
				//return false;
			}
			DFS_GetVolInfo(0, scratch_sector, p->start_sector, a);
			p++;
			a++;
		}
	}
	else
	{
		if (DFS_GetVolInfo(0, scratch_sector, 0, arch->vi)) {
			//printf("Error getting volume information\n");
			ArchiveData->OpenResult = E_BAD_DATA;
			return false;
		}
	}

	ArchiveData->OpenResult = 0;// ok

	return true;
}

tArchive* Open(tOpenArchiveData* ArchiveData)
{
	int partitions = 1;
	tArchive* arch = NULL;

	if ((arch = new tArchive) == NULL)
	{
		return NULL;
	}
	memset(arch, 0, sizeof(tArchive));
	arch->volume_dirty = false;
	strcpy_s(arch->archname,MAX_PATH, ArchiveData->ArcName);
	pCurrentArchive = arch;

	// trying to open
	if (!OpenImage(ArchiveData, arch))
		goto error;

	entryList.next = NULL;
	entryList.prev = NULL;

	char path[MAX_PATH + 1];

	uint32_t result;
	if (disk_image.mode == DISKMODE_HARD_DISK)
	{
		partitions = MAX_PARTITIONS;
	}

	for (int i = 0; i < partitions; i++)
	{
		path[0] = 0;
		result = scan_files((char *)&path, &arch->vi[i], i);
		if (result != DFS_OK && result != DFS_EOF) {
			arch->currentEntry = &entryList;
			arch->lastEntry = NULL;
			ArchiveData->OpenResult = E_BAD_DATA;
			return arch;
		}
	}
	arch->currentEntry = &entryList;
	arch->lastEntry = NULL;
	ArchiveData->OpenResult = 0;// ok
	return arch;

error:
	// memory must be freed
	delete arch;
	return NULL;
};

int NextItem(tArchive* hArcData, tHeaderData* HeaderData)
{
	tArchive* arch = (tArchive*)(hArcData);
	if (arch->currentEntry->next == NULL) {
		return E_BAD_ARCHIVE;
	}
	
	strcpy_s(HeaderData->ArcName,MAX_PATH, arch->archname);
	strcpy_s(HeaderData->FileName, MAX_PATH, arch->currentEntry->fileWPath);
	/*
	0x1 Read-only file
	0x2 Hidden file
	0x4 System file
	0x8 Volume ID file
	0x10 Directory
	0x20 Archive file
	*/
	HeaderData->FileAttr = arch->currentEntry->de.attr;
	// TC format: FileTime = (year - 1980) << 25 | month << 21 | day << 16 | hour << 11 | minute << 5 | second / 2;
	// FAT entries: 
	// Date: F E D C B A 9 8 7 6 5 4 3 2 1 0    Time: F E D C B A 9 8 7 6 5 4 3 2 1 0
	//      | Year        | Month | Day     |        | Hour    | Minute    | Second  |
	HeaderData->FileTime = (arch->currentEntry->de.wrttime_h << 8) | (arch->currentEntry->de.wrttime_l) | (arch->currentEntry->de.wrtdate_h << 24) | (arch->currentEntry->de.wrtdate_l << 16);
	HeaderData->PackSize = arch->currentEntry->de.filesize_0 | (arch->currentEntry->de.filesize_1 << 8) | (arch->currentEntry->de.filesize_2 << 16) | (arch->currentEntry->de.filesize_3 << 24);
	HeaderData->UnpSize = HeaderData->PackSize;
	HeaderData->CmtBuf = 0;
	HeaderData->CmtBufSize = 0;
	HeaderData->CmtSize = 0;
	HeaderData->CmtState = 0;
	HeaderData->UnpVer = 0;
	HeaderData->Method = 0;
	HeaderData->FileCRC = 0;

	arch->lastEntry = arch->currentEntry;

	if (arch->currentEntry->next == NULL) {
		return E_END_ARCHIVE;
	}
	arch->currentEntry = arch->currentEntry->next;
	return 0;//ok
};

int Process(tArchive* hArcData, int Operation, char* DestPath, char* DestName)
{
	uint8_t scratch_sector[SECTOR_SIZE];
	if (Operation == PK_SKIP || Operation == PK_TEST) return 0;
	tArchive *arch = hArcData;

	// TODO: This is here for now to disallow people from messing up .DIM images.
	//       It will go away eventually once we implement .DIM creation
	if (disk_image.mode == DISKMODE_FCOPY_CONF_ALL_SECTORS || disk_image.mode == DISKMODE_FCOPY_NO_CONF || disk_image.mode == DISKMODE_FCOPY_CONF_USED_SECTORS)
	{
		return E_NOT_SUPPORTED;
	}

	int filename_offset = 0;
	if (disk_image.mode == DISKMODE_HARD_DISK)
	{
		// Strip out the partition path prefix (for now it's "0\", "1\", etc depending on partition)
		filename_offset = 2;
	}

	if (Operation == PK_EXTRACT && arch->lastEntry != NULL) {
		uint32_t res;
		FILEINFO fi;
		res = DFS_OpenFile(arch->vi, (uint8_t *)arch->lastEntry->fileWPath + filename_offset, DFS_READ, scratch_sector, &fi, 0);
		if (res != DFS_OK) {
			return E_EREAD;
		}
		unsigned int readLen;
		unsigned int len;
		len = fi.filelen;
		unsigned char *buf = (uint8_t *)calloc(1, len + 1024); // Allocate some extra RAM and wipe it so we don't write undefined values to the file
		res = DFS_ReadFile(&fi, scratch_sector, buf, &readLen, len);
		if (res != DFS_OK) {
			free(buf);
			return E_EREAD;
		}
		if (DestPath == NULL) {
			// DestName contains the full path and file nameand DestPath is NULL
			FILE* f;
			fopen_s(&f,DestName, "wb");
			if (f == NULL) {
				free(buf);
				return E_EWRITE;
			}
			size_t wlen = fwrite(buf, 1, len, f);
			if (wlen != len) {
				free(buf);
				fclose(f);
				return E_EWRITE;
			}
			fclose(f);
		}
		else {
			// DestName contains only the file name and DestPath the file path
			char file[MAX_PATH];
			sprintf_s(file,MAX_PATH, "%s" PATH_SEPARATOR "%s", DestPath, DestName);
			FILE* f;
			fopen_s(&f,file, "wb");
			if (f == NULL) {
				free(buf);
				return E_EWRITE;
			}
			size_t wlen = fwrite(buf, 1, len, f);
			if (wlen != len) {
				free(buf);
				fclose(f);
				return E_EWRITE;
			}
			fclose(f);
		}
		free(buf);
		return 0;
	}

	return E_BAD_DATA;//ok
};


int Close(tArchive* hArcData)
{
	tArchive* arch = (tArchive*)(hArcData);
	pCurrentArchive = NULL;
	DFS_HostDetach(arch);
	stEntryList* entry = &entryList;
	while (entry->next != NULL) {
		if (entry->prev != NULL && entry->prev != &entryList) {
			delete entry->prev;
		}
		entry = entry->next;
	}
	delete arch;

	return 0;// ok
};

// TODO: totally reject long filenames for now, as DOSFS is having a really bad time with them
int Pack(char *PackedFile, char *SubPath, char *SrcPath, char *AddList, int Flags)
{
	if (!AddList || *AddList == 0) return E_NO_FILES;
	tOpenArchiveData archive_data = { 0 };
	tArchive archive_handle = { 0 };
	strcpy(archive_handle.archname, PackedFile);
	bool status = OpenImage(&archive_data, &archive_handle);
	if (!status)
	{
		// This is what Open() returns if it reaches an error (archive_handle=NULL).
		// However, since the return sctruct is deleted before returned, we pass this error manually here
		return E_BAD_ARCHIVE;
	}

	if (archive_data.OpenResult)
	{
		return archive_data.OpenResult;
	}

	int partition = 0;
	if (disk_image.mode == DISKMODE_HARD_DISK)
	{
		// Determine which partition we are going to write to.
		partition = *SubPath - '0';

		// Strip out the partition path prefix (for now it's "0", "1", etc depending on partition)
		SubPath++;
	}

	uint32_t res;
	FILEINFO fi;
	uint8_t scratch_sector[SECTOR_SIZE];
	char filename_source[MAX_PATH];
	char filename_dest[MAX_PATH];
	if (Flags & PK_PACK_SAVE_PATHS)
	{

	}
	if (SubPath && *SubPath)
	{
		strcpy(filename_dest, SubPath);
		strcat(filename_dest, PATH_SEPARATOR);
	}
	else
	{
		strcpy(filename_dest, PATH_SEPARATOR);
	}
	strcpy(filename_source, SrcPath);
	char *filename_subpath = filename_source + strlen(filename_source);
	char *current_file = AddList;
	while (*current_file) // Each string in AddList is zero-delimited (ends in zero), and the AddList string ends with an extra zero byte, i.e. there are two zero bytes at the end of AddList.
	{
		struct tm *file_tm;
		struct _stat file_stats;
		int file_timestamp;
		int file_size;
		unsigned char *read_buf;
		size_t items_read;
		unsigned int bytes_written;
		if (current_file[strlen(current_file) - 1] == '\\')
		{
			// New folder to be created
			current_file[strlen(current_file) - 1] = 0; // Remove the trailing '\' as to be not confused by a path
			uint8_t *buf = (uint8_t *)calloc(1, SECTOR_SIZE * 2);
			if (!buf) return E_ECREATE;
			// Create the directory entry in the parent directory
			time_t ltime;
			time(&ltime);	// Get current date/time
			file_tm = localtime(&ltime);
			file_timestamp = ((file_tm->tm_year - 80) << 25) | (file_tm->tm_wday << 21) | (file_tm->tm_mday << 16) | (file_tm->tm_hour << 11) | (file_tm->tm_min << 5) | ((file_tm->tm_sec / 2));
			res = DFS_OpenFile(&archive_handle.vi[partition], (uint8_t *)current_file, DFS_WRITE | DFS_FOLDER, scratch_sector, &fi, file_timestamp);
			if (res != DFS_OK) {
				DFS_HostDetach(&archive_handle);
				return E_ECREATE;
			}
			// Now, create the "." and ".." entries in the new folder
			DIRENT *de = (DIRENT *)buf;
			strcpy((char *)de->name, ".          \x10");
			de->crtdate_h = (uint8_t)(file_timestamp >> 24);
			de->crtdate_l = (uint8_t)(file_timestamp >> 16);
			de->crttime_h = (uint8_t)(file_timestamp >> 8);
			de->crttime_l = (uint8_t)file_timestamp;
			de->wrtdate_h = (uint8_t)(file_timestamp >> 24);
			de->wrtdate_l = (uint8_t)(file_timestamp >> 16);
			de->wrttime_h = (uint8_t)(file_timestamp >> 8);
			de->wrttime_l = (uint8_t)file_timestamp;
			de++;
			strcpy((char *)de->name, "..         \x10");
			de->crtdate_h = (uint8_t)(file_timestamp >> 24);
			de->crtdate_l = (uint8_t)(file_timestamp >> 16);
			de->crttime_h = (uint8_t)(file_timestamp >> 8);
			de->crttime_l = (uint8_t)file_timestamp;
			de->wrtdate_h = (uint8_t)(file_timestamp >> 24);
			de->wrtdate_l = (uint8_t)(file_timestamp >> 16);
			de->wrttime_h = (uint8_t)(file_timestamp >> 8);
			de->wrttime_l = (uint8_t)file_timestamp;
			res = DFS_WriteFile(&fi, scratch_sector, buf, &bytes_written, SECTOR_SIZE*2);
			free(buf);
			if (res != DFS_OK) {
				DFS_HostDetach(&archive_handle);
				return E_ECREATE;
			}
			// Point to next item in the list (although there probably won't be one)
			current_file += strlen(current_file) + 2;
			continue;
		}
		strcpy(filename_subpath, current_file);
		FILE *handle_to_add=fopen(filename_source, "rb");
		if (!handle_to_add)
		{
			return E_NO_FILES;
		}
		_stat(filename_source, &file_stats);
		file_tm = localtime(&file_stats.st_mtime);
		file_timestamp = ((file_tm->tm_year-80) << 25) | (file_tm->tm_wday << 21) | (file_tm->tm_mday << 16) | (file_tm->tm_hour << 11) | (file_tm->tm_min << 5) | ((file_tm->tm_sec / 2));
		fseek(handle_to_add, 0, SEEK_END);
		file_size = ftell(handle_to_add);
		if (file_size < 0) {
			return E_NO_FILES;
		}
		read_buf = (unsigned char *)calloc(1, file_size + 1024); // Allocate some extra RAM and wipe it so we don't write undefined values to the file
		if (!read_buf)
		{
			DFS_HostDetach(&archive_handle);
			return E_NO_MEMORY;
		}
		if (file_size) // don't read for 0 sized files
		{
			fseek(handle_to_add, 0, SEEK_SET);
			items_read = fread(read_buf, file_size, 1, handle_to_add);
			if (!items_read)
			{
				fclose(handle_to_add);
				free(read_buf);
				DFS_HostDetach(&archive_handle);
				return E_EREAD;
			}
		}
		fclose(handle_to_add);
		strcpy(&filename_dest[1], current_file);
		res = DFS_OpenFile(&archive_handle.vi[partition], (uint8_t *)filename_dest, DFS_WRITE, scratch_sector, &fi, file_timestamp);
		if (res != DFS_OK)
		{
			free(read_buf);
			DFS_HostDetach(&archive_handle);
			return E_ECREATE;
		}
		res = DFS_WriteFile(&fi, scratch_sector, read_buf, &bytes_written, file_size);
		if (res != DFS_OK)
		{
			free(read_buf);
			DFS_HostDetach(&archive_handle);
			return E_EWRITE;
		}
		if (bytes_written != file_size)
		{
			// Out of disk space - unsure what error to return here
			free(read_buf);
			DFS_HostDetach(&archive_handle);
			return E_TOO_MANY_FILES;
		}
		free(read_buf);
		// Point to next file (or NULL termination)
		current_file += strlen(current_file) + 1;
	}
	archive_handle.volume_dirty = true;
	DFS_HostDetach(&archive_handle);
	if (Flags & PK_PACK_MOVE_FILES)
	{
		LPCSTR delete_list = (LPCSTR)AddList;

		while (*delete_list)
		{
#ifdef _WIN32
			if (!DeleteFileA(delete_list))
#else
			if (unlink(delete_list))
#endif
			{
				return E_ECLOSE;	// No idea what would be the correct error code
			}
			delete_list += strlen(delete_list) + 1;
		}
	}
	return 0; // All ok
}

uint32_t scan_folder_and_delete(PVOLINFO vi, char *path)
{
	uint8_t *scratch_dir = (uint8_t *)malloc(SECTOR_SIZE);
	if (!scratch_dir) return DFS_ERRMISC;
	uint8_t *scratch_delete = (uint8_t *)malloc(SECTOR_SIZE);
	if (!scratch_delete) return DFS_ERRMISC;

	PDIRINFO di = (PDIRINFO)malloc(sizeof(DIRINFO));
	if (!di) return DFS_ERRMISC;
	di->scratch = scratch_dir;

	uint32_t ret;
	
	ret = DFS_OpenDir(vi, (uint8_t *)path, di);
	if (ret != DFS_OK) return ret;
	char filename_canonical[13];

	DIRENT de;

	do
	{
		ret = DFS_GetNext(vi, di, &de);

		if (ret == DFS_EOF) break;

		if (de.name[0] == 0) continue;
		if (strcmp((char *)de.name, ".          \x10") == 0) continue;
		if (strcmp((char *)de.name, "..         \x10") == 0) continue;
		DirToCanonical(filename_canonical, de.name);
		if (de.attr & ATTR_VOLUME_ID)
		{
			continue;
		}
		if (de.attr & ATTR_DIRECTORY)
		{
			char new_path[MAX_PATH + 1];
			sprintf(new_path, "%s\\%s", path, de.name);
			ret = scan_folder_and_delete(vi, new_path);

			if (ret != DFS_OK && ret != DFS_EOF)
			{
				break;
			}

		}
		else
		{
			uint8_t filename_to_delete[MAX_PATH + 1];
			if (strlen(path) + strlen(filename_canonical) + 1 >= MAX_PATH ||
				sprintf_s((char *const )filename_to_delete, MAX_PATH, "%s\\%s", path, filename_canonical) == -1)
			{
				ret = DFS_ERRMISC;
				break;
			}

			ret = DFS_UnlinkFile(vi, filename_to_delete, scratch_delete);
			if (ret != DFS_OK)
			{
				break;
			}

		}
	} while (1);

	if (ret == DFS_EOF)
	{
		// Delete the actual folder
		ret = DFS_UnlinkFile(vi, (uint8_t *)path, scratch_dir);
	}

	free(scratch_dir);
	free(scratch_delete);
	free(di);
	return ret;
}

int Delete(char *PackedFile, char *DeleteList)
{
	if (!DeleteList || !*DeleteList) return E_NO_FILES;

	tOpenArchiveData archive_data = { 0 };
	archive_data.ArcName = PackedFile;
	tArchive archive_handle = { 0 };

	strcpy(archive_handle.archname, PackedFile);
	bool status = OpenImage(&archive_data, &archive_handle);
	if (!status)
	{
		// This is what Open() returns if it reaches an error (archive_handle=NULL).
		// However, since the return sctruct is deleted before returned, we pass this error manually here
		return E_BAD_ARCHIVE;
	}

	if (archive_data.OpenResult)
	{
		return archive_data.OpenResult;
	}

	uint32_t res;
	uint8_t scratch_sector[SECTOR_SIZE];

	while (*DeleteList) // Each string in AddList is zero-delimited (ends in zero), and the AddList string ends with an extra zero byte, i.e. there are two zero bytes at the end of AddList.
	{
		int partition = 0;
		if (disk_image.mode == DISKMODE_HARD_DISK)
		{
			// Determine which partition we are going to write to.
			partition = *DeleteList - '0';

			// Strip out the partition path prefix (for now it's "0", "1", etc depending on partition)
			DeleteList += 2;
		}

		if (strlen(DeleteList) > 4 && strcmp(&DeleteList[strlen(DeleteList) - 3], "*.*") == 0)
		{
			// We have to delete a folder. This is more tricky as we need to ensure that the folder
			// is empty first. Which means we have to recursively scan and delete all the things
			DeleteList[strlen(DeleteList) - 4] = 0; // Remove the "\*.*" postfix
			res = scan_folder_and_delete(&archive_handle.vi[partition], DeleteList);
			DeleteList += strlen(DeleteList) + 1; // Point to "*.*" which we chopped out above, so we can then point to the next item to delete (if any)
		}
		else
		{
			res = DFS_UnlinkFile(&archive_handle.vi[partition], (uint8_t *)DeleteList, scratch_sector);
		}

		if (res != DFS_OK)
		{
			DFS_HostDetach(&archive_handle);
			return E_ECLOSE;
		}

		DeleteList += strlen(DeleteList) + 1;
	}
	archive_handle.volume_dirty = true;
	DFS_HostDetach(&archive_handle);

	return 0; // All ok
}

#ifndef _WIN32
extern "C"
{
#endif

// OpenArchive should perform all necessary operations when an archive is to be opened
myHANDLE __stdcall OpenArchive(tOpenArchiveData* ArchiveData)
{
	return Open(ArchiveData);
}

// WinCmd calls ReadHeader to find out what files are in the archive
int __stdcall ReadHeader(myHANDLE hArcData, tHeaderData* HeaderData)
{
	return NextItem(hArcData, HeaderData);
}

// ProcessFile should unpack the specified file or test the integrity of the archive
int __stdcall ProcessFile(myHANDLE hArcData, int Operation, char* DestPath, char* DestName)
{
	return Process(hArcData, Operation, DestPath, DestName);
}

// CloseArchive should perform all necessary operations when an archive is about to be closed
int __stdcall CloseArchive(myHANDLE hArcData)
{
	return Close(hArcData);
}

// Add/Move files to image
int __stdcall PackFiles(char *PackedFile, char *SubPath, char *SrcPath, char *AddList, int Flags)
{
	return Pack(PackedFile, SubPath, SrcPath, AddList, Flags);
}

// Delete files from image
int __stdcall DeleteFiles(char *PackedFile, char *DeleteList)
{
	return Delete(PackedFile, DeleteList);
}

// This function allows you to notify user about changing a volume when packing files
void __stdcall SetChangeVolProc(myHANDLE hArcData, tChangeVolProc pChangeVolProc)
{
//	IMG_SetCallBackVol(hArcData, pChangeVolProc);
}

// This function allows you to notify user about the progress when you un/pack files
void __stdcall SetProcessDataProc(myHANDLE hArcData, tProcessDataProc pProcessDataProc)
{
//	IMG_SetCallBackProc(hArcData, pProcessDataProc);
}

int __stdcall GetPackerCaps() {
	return PK_CAPS_SEARCHTEXT | PK_CAPS_BY_CONTENT | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE | PK_CAPS_NEW;
}

BOOL __stdcall CanYouHandleThisFile(char* FileName) {
	//we simply can't check .ST files by contents, so fake checking it by actually opening it
	//do the same for .MSA for good measure
	if ((strlen(FileName) > 3 && _strcmpi(FileName + strlen(FileName) - 3, ".st") == 0) || 
		(strlen(FileName) > 4 && _strcmpi(FileName + strlen(FileName) - 4, ".msa") == 0) ||
		(strlen(FileName) > 4 && _strcmpi(FileName + strlen(FileName) - 4, ".dim") == 0) ||
		(strlen(FileName) > 4 && _strcmpi(FileName + strlen(FileName) - 4, ".lol") == 0)) {
		tOpenArchiveData oad;
		oad.ArcName = FileName;
		tArchive* pa = Open(&oad);
		if (pa == NULL) {
			return false;
		}
		if (oad.OpenResult != 0) {
			return false;
		}
		Close(pa);
		return true;
	}
	return false;
}

#ifndef _WIN32
}
#endif

#ifdef _WIN32
// The DLL entry point
BOOL APIENTRY DllMain(HANDLE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    return TRUE;
}
#endif
