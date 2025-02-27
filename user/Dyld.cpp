#include "Dyld.hpp"
#include "Kernel.hpp"
#include "MachO.hpp"
#include "Task.hpp"
#include "Offset.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <mach/mach.h>

static int EndsWith(const char *str, const char *suffix)
{
	if (!str || !suffix)
		return 0;
	
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	
	if (lensuffix >  lenstr)
		return 0;
	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}

Dyld::Dyld(Kernel *kernel, Task *task)
{
	bool found_main_image = false;

	struct mach_header_64 hdr;

	struct dyld_all_image_infos all_images;

	this->kernel = kernel;
	this->task = task;

	this->all_image_info_addr = kernel->read64(task->getTask() + Offset::task_all_image_info_addr);
	this->all_image_info_size = kernel->read64(task->getTask() + Offset::task_all_image_info_size);

	if(!all_image_info_addr || !all_image_info_size)
	{
		kern_return_t kr;

		struct task_dyld_info dyld_info;

		mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;

		if((kr = task_info(task->getTaskPort(), TASK_DYLD_INFO, (task_info_t) &dyld_info, &count)) == KERN_SUCCESS)
		{
			this->all_image_info_addr = dyld_info.all_image_info_addr;
			this->all_image_info_size = dyld_info.all_image_info_size;
		} else
		{
			MAC_RK_LOG("MacRK::could not find all_image_info for task! %d\n", kr);
		}
	}

	assert(this->all_image_info_addr && this->all_image_info_size);

	task->read(all_image_info_addr, &all_images, sizeof(struct dyld_all_image_infos));

	this->all_image_infos = reinterpret_cast<struct dyld_all_image_infos*> (malloc(sizeof(struct dyld_all_image_infos)));

	memcpy(this->all_image_infos, &all_images, sizeof(struct dyld_all_image_infos));

	for(uint32_t i = 0; i < all_image_infos->infoArrayCount; i++)
	{
		struct dyld_image_info image_info;

		mach_vm_address_t image_info_addr;

		mach_vm_address_t image_load_addr;
		mach_vm_address_t image_file_path;

		char *image_file;

		image_info_addr = (mach_vm_address_t) (all_images.infoArray + i);

		task->read(image_info_addr, &image_info, sizeof(image_info));

		image_load_addr = (mach_vm_address_t) image_info.imageLoadAddress;
		image_file_path = (mach_vm_address_t) image_info.imageFilePath;

		image_file = task->readString(image_file_path);

		if(!found_main_image && EndsWith(image_file, task->getName()))
		{
			task->read(image_load_addr, &hdr, sizeof(hdr));

			if(hdr.magic == MH_MAGIC_64)
			{
				this->main_image_info = (struct dyld_image_info*) malloc(sizeof(struct dyld_image_info));

				memcpy(this->main_image_info, &image_info, sizeof(image_info));

				this->dyld_shared_cache = this->all_image_infos->sharedCacheBaseAddress;

				printf("%s main image loaded at 0x%llx\n", image_file, image_load_addr);

				this->main_image_load_base = image_load_addr;

				found_main_image = true;
			}
		}

		free(image_file);
	}

	assert(found_main_image);

	this->dyld = this->getImageLoadedAt("libdyld.dylib", NULL);
	this->dyld_shared_cache = this->all_image_infos->sharedCacheBaseAddress;

	printf("dyld_shared_cache = 0x%llx\n", this->dyld_shared_cache);

	assert(this->dyld);
	assert(this->dyld_shared_cache);
}

Dyld::~Dyld()
{
	
}

mach_vm_address_t Dyld::getImageLoadedAt(char *image_name, char **image_path)
{
	struct mach_header_64 hdr;

	struct dyld_all_image_infos all_images;

	task->read(this->all_image_info_addr, &all_images, sizeof(all_images));

	if(!this->all_image_infos)
	{
		this->all_image_infos = reinterpret_cast<struct dyld_all_image_infos*> (malloc(sizeof(struct dyld_all_image_infos)));
	}

	memcpy(this->all_image_infos, &all_images, sizeof(struct dyld_all_image_infos));

	for(uint32_t i = 0; i < all_image_infos->infoArrayCount; i++)
	{
		struct dyld_image_info image_info;

		mach_vm_address_t image_info_addr;

		mach_vm_address_t image_load_addr;
		mach_vm_address_t image_file_path;

		char *image_file;

		image_info_addr = (mach_vm_address_t) (all_image_infos->infoArray + i);

		task->read(image_info_addr, &image_info, sizeof(image_info));

		image_load_addr = (mach_vm_address_t) image_info.imageLoadAddress;
		image_file_path = (mach_vm_address_t) image_info.imageFilePath;

		image_file = task->readString(image_file_path);

		if(EndsWith(image_file, image_name))
		{
			task->read(image_load_addr, &hdr, sizeof(hdr));

			if(hdr.magic == MH_MAGIC_64)
			{
				if(image_path)
					*image_path = image_file;

				return image_load_addr;
			}
		}

		free(image_file);
	}

	if(image_path)
		*image_path = NULL;

	return 0;
}

struct dyld_cache_header* Dyld::cacheGetHeader()
{
	struct dyld_cache_header *cache_header;

	mach_vm_address_t shared_cache_rx_base;

	shared_cache_rx_base = this->dyld_shared_cache;

	cache_header = reinterpret_cast<struct dyld_cache_header*> (malloc(sizeof(struct dyld_cache_header)));

	task->read(shared_cache_rx_base, cache_header, sizeof(struct dyld_cache_header));

	return cache_header;
}

struct dyld_cache_mapping_info* Dyld::cacheGetMappings(struct dyld_cache_header *cache_header)
{
	struct dyld_cache_mapping_info *mappings;

	mappings = reinterpret_cast<struct dyld_cache_mapping_info*>(malloc(sizeof(struct dyld_cache_mapping_info) * cache_header->mappingCount));

	task->read(this->dyld_shared_cache + cache_header->mappingOffset, mappings, sizeof(struct dyld_cache_mapping_info) * cache_header->mappingCount);

	return mappings;
}

struct dyld_cache_mapping_info* Dyld::cacheGetMapping(struct dyld_cache_header *cache_header, vm_prot_t prot)
{
	for(uint32_t i = 0; i < cache_header->mappingCount; ++i)
	{
		struct dyld_cache_mapping_info *mapping = (struct dyld_cache_mapping_info*) malloc(sizeof(struct dyld_cache_mapping_info));

		task->read(this->dyld_shared_cache + cache_header->mappingOffset + sizeof(struct dyld_cache_mapping_info) * i, mapping, sizeof(struct dyld_cache_mapping_info));

		if(mapping->initProt == prot)
		{
			printf("Index of returned mapping = %u\n", i);

			return mapping;
		}

		free(mapping);
	}

	return NULL;
}

void Dyld::cacheOffsetToAddress(uint64_t dyld_cache_offset, mach_vm_address_t *address, off_t *aslr)
{
	struct dyld_cache_header *cache_header;

	struct dyld_cache_mapping_info *mappings;

	struct dyld_cache_mapping_info *rx;
	struct dyld_cache_mapping_info *ro;
	struct dyld_cache_mapping_info *rw;

	mach_vm_address_t shared_cache_rx_base;

	shared_cache_rx_base = this->dyld_shared_cache;

	cache_header = this->cacheGetHeader();

	printf("magic = %s cache mapping count = %u \n", cache_header->magic, cache_header->mappingCount);

	mappings = this->cacheGetMappings(cache_header);

	/*
	rx = this->cacheGetMapping(cache_header, VM_PROT_READ | VM_PROT_EXECUTE);

	ro = this->cacheGetMapping(cache_header, VM_PROT_READ);

	rw = this->cacheGetMapping(cache_header, VM_PROT_READ | VM_PROT_WRITE);
	*/

	size_t rx_size = 0;
	size_t rw_size = 0;
	size_t ro_size = 0;

	mach_vm_address_t rx_addr = 0;
	mach_vm_address_t rw_addr = 0;
	mach_vm_address_t ro_addr = 0;

	mach_vm_address_t dyld_cache_address = 0;

	for(uint32_t i = 0; i < cache_header->mappingCount; i++)
	{
		struct dyld_cache_mapping_info *mapping = &mappings[i];

		off_t low_bound = mapping->fileOffset;
		off_t high_bound = mapping->fileOffset + mapping->size;

		if(dyld_cache_offset >= low_bound &&
		   dyld_cache_offset < high_bound)
		{
			mach_vm_address_t mappingAddr = dyld_cache_offset - low_bound;

			dyld_cache_address = mappingAddr + mapping->address;
		}

		printf("mapping %u\n", i);
		printf("\taddress = 0x%llx prot = %u offset = 0x%llx size = 0x%llx\n", mapping->address, mapping->maxProt, mapping->fileOffset, mapping->size);

		if(mapping->maxProt == (VM_PROT_READ | VM_PROT_EXECUTE))
		{
			rx_size += mapping->size;
			rx_addr = mapping->address;
		}

		if(mapping->maxProt == (VM_PROT_READ | VM_PROT_WRITE))
		{
			rw_size += mapping->size;
			rw_addr = mapping->address;
		}

		if(mapping->maxProt == VM_PROT_READ)
		{
			ro_size += mapping->size;
			ro_addr = mapping->address;
		}
	}

	printf("dyld_cache_address = 0x%llx\n", dyld_cache_address);

	off_t aslr_slide = (off_t) (this->dyld_shared_cache - rx_addr);

	char *shared_cache_ro = (char*)((mach_vm_address_t) ro_addr + aslr_slide);

	off_t offset_from_ro = (off_t) (dyld_cache_offset - rx_size - rw_size);

	if(address)
		*address = ((mach_vm_address_t) dyld_cache_address + aslr_slide);

	if(slide)
		*aslr = aslr_slide;

	printf("Slide = 0x%llx\n", aslr_slide);

	printf("Dyld address = 0x%llx calculated\n", *address);

	free(cache_header);

	// free(rx);
	// free(ro);
	//free(rw);
}

void Dyld::cacheGetSymtabStrtab(struct symtab_command *symtab_command, mach_vm_address_t *symtab, mach_vm_address_t *strtab, off_t *slide)
{
	 this->cacheOffsetToAddress(symtab_command->symoff,
									symtab,
									slide);

	 this->cacheOffsetToAddress(symtab_command->stroff,
									strtab,
									slide);

	this->slide = *slide;
}

mach_vm_address_t Dyld::getImageSlide(mach_vm_address_t address)
{
	bool ok;

	struct mach_header_64 *hdr;

	mach_vm_address_t symtab;
	mach_vm_address_t strtab;

	off_t slide;

	uint8_t *cmds;

	uint32_t ncmds;
	size_t sizeofcmds;

	uint64_t cmd_offset;

	hdr = reinterpret_cast<struct mach_header_64*>(malloc(sizeof(struct mach_header_64)));

	ok = this->task->read(address, hdr, sizeof(struct mach_header_64));

	ncmds = hdr->ncmds;
	sizeofcmds = hdr->sizeofcmds;

	cmds = reinterpret_cast<uint8_t*>(malloc(sizeofcmds));

	ok = this->task->read(address + sizeof(struct mach_header_64), cmds, sizeofcmds);

	cmd_offset = 0;

	slide = 0;

	for(int i = 0; i < ncmds; i++)
	{
		struct load_command *load_cmd = reinterpret_cast<struct load_command*>(cmds + cmd_offset);

		uint32_t cmdtype = load_cmd->cmd;
		uint32_t cmdsize = load_cmd->cmdsize;

		switch(cmdtype)
		{
			case LC_SYMTAB:
			{
				;
				struct symtab_command *symtab_command = (struct symtab_command*) load_cmd;

				this->cacheGetSymtabStrtab(symtab_command, &symtab, &strtab, &slide);

				break;
			}
		}

		cmd_offset += cmdsize;
	}

	free(hdr);
	free(cmds);

	return slide;
}

size_t Dyld::getAdjustedStrtabSize(mach_vm_address_t linkedit, off_t linkedit_fileoff, struct symtab_command *symtab_command)
{
	mach_vm_address_t symtab;
	mach_vm_address_t strtab;

	size_t symsize;
	size_t new_strsize;

	off_t symoff;
	off_t stroff;

	off_t slide;

	struct nlist_64 *syms;

	symoff = symtab_command->symoff;
	stroff = symtab_command->stroff;

	symsize = symtab_command->nsyms * sizeof(struct nlist_64);

	syms = reinterpret_cast<struct nlist_64*>(malloc(symsize));

	this->cacheGetSymtabStrtab(symtab_command, &symtab, &strtab, &slide);

	symtab = linkedit + slide + (symtab_command->symoff - linkedit_fileoff);
	strtab = linkedit + slide + (symtab_command->stroff - linkedit_fileoff);

	this->task->read(symtab, syms, symsize);

	new_strsize = 0;

	for(int i = 0; i < symtab_command->nsyms ; i++)
	{
		struct nlist_64* nl = &syms[i];

		char *sym = this->task->readString(strtab + nl->n_strx);

		new_strsize += strlen(sym) + 1;

		free(sym);
	}

	free(syms);

	return new_strsize;
}

size_t Dyld::getAdjustedLinkeditSize(mach_vm_address_t address)
{
	struct mach_header_64 *hdr;

	size_t linkedit_new_sz;

	mach_vm_address_t symtab;
	mach_vm_address_t strtab;

	mach_vm_address_t linkedit_vmaddr;
	mach_vm_address_t text_vmaddr;

	mach_vm_address_t linkedit_begin_off;

	mach_vm_address_t linkedit_old_off;
	mach_vm_address_t linkedit_new_off;

	off_t slide;

	mach_vm_address_t align;
	mach_vm_address_t current_offset;

	uint64_t cmd_offset;

	uint8_t *cmds;
	uint8_t *q;

	uint32_t ncmds;
	size_t sizeofcmds;

	size_t filesz = 0;

	slide = this->getImageSlide(address);

	linkedit_new_sz = 0;

	hdr = reinterpret_cast<struct mach_header_64*>(malloc(sizeof(struct mach_header_64)));

	bool ok = this->task->read(address, hdr, sizeof(struct mach_header_64));

	assert(ok);

	ncmds = hdr->ncmds;
	sizeofcmds = hdr->sizeofcmds;

	cmds = reinterpret_cast<uint8_t*>(malloc(sizeofcmds));

	ok = this->task->read(address + sizeof(struct mach_header_64), cmds, sizeofcmds);

	align = sizeof(struct mach_header_64) + hdr->sizeofcmds;

	current_offset = (align % 0x1000 > 0 ? align / 0x1000 + 1 : align / 0x1000) * 0x1000;

	q = cmds;

	for(int i = 0; i < ncmds; i++)
	{
		struct load_command *load_cmd = reinterpret_cast<struct load_command*>(q);

		uint32_t cmdtype = load_cmd->cmd;
		uint32_t cmdsize = load_cmd->cmdsize;

		if(load_cmd->cmd == LC_SEGMENT_64)
		{
			struct segment_command_64 *segment_command = reinterpret_cast<struct segment_command_64*>(load_cmd);
			
			if(strcmp(segment_command->segname, "__LINKEDIT") != 0)
			{
				current_offset += segment_command->filesize;
			}
			else
			{
				linkedit_begin_off = current_offset;

				linkedit_new_off = current_offset;
				linkedit_old_off = segment_command->fileoff;

				linkedit_vmaddr = segment_command->vmaddr;
			}

			printf("LC_SEGMENT_64 at 0x%llx - %s 0x%08llx to 0x%08llx \n", segment_command->fileoff,
		                                  segment_command->segname,
		                                  segment_command->vmaddr,
		                                  segment_command->vmaddr + segment_command->vmsize);

			uint64_t sect_offset = 0;

			for(int j = 0; j < segment_command->nsects; j++)
			{
				struct section_64 *section = (struct section_64*) ((uint64_t) load_cmd + sizeof(struct segment_command_64) + sect_offset);

				printf("\tSection %d: 0x%08llx to 0x%08llx - %s\n", j,
				                            section->addr,
				                            section->addr + section->size,
				                            section->sectname);

				sect_offset += sizeof(struct section_64);
			}
		} else if(load_cmd->cmd == LC_SYMTAB)
		{
			struct symtab_command *symtab_command = reinterpret_cast<struct symtab_command*>(q);

			ok = this->task->read(address + sizeof(struct mach_header_64), cmds, sizeofcmds);

			printf("linkedit file off = 0x%llx\n", linkedit_old_off);
			printf("symoff 0x%x stroff 0x%x\n", symtab_command->symoff, symtab_command->stroff);

			uint32_t new_strsize = this->getAdjustedStrtabSize(linkedit_vmaddr, linkedit_old_off, symtab_command);

			uint64_t symsize = symtab_command->nsyms * sizeof(struct nlist_64);
			uint64_t strsize = symtab_command->strsize;

			uint64_t old_symoff = symtab_command->symoff;
			uint64_t old_stroff = symtab_command->stroff;

			uint64_t new_symoff = (old_symoff - linkedit_old_off);
			uint64_t new_stroff = (old_stroff - linkedit_old_off);

			// symtab_command->symoff = (uint32_t) new_symoff;
			// symtab_command->stroff = (uint32_t) new_stroff;

			linkedit_new_sz += (symsize + new_strsize);

			new_symoff = (old_symoff - linkedit_old_off) + linkedit_new_off;
			new_stroff = (old_stroff - linkedit_old_off) + linkedit_new_off;

			// symtab_command->symoff = (uint32_t) new_symoff;
			// symtab_command->stroff = (uint32_t) new_stroff;

		} else if(load_cmd->cmd == LC_DYSYMTAB)
		{
			struct dysymtab_command *dysymtab_command = (struct dysymtab_command*) q;

			uint32_t tocoff = dysymtab_command->tocoff;
			uint32_t tocsize = dysymtab_command->ntoc * sizeof(struct dylib_table_of_contents);

			uint32_t modtaboff = dysymtab_command->modtaboff;
			uint32_t modtabsize = dysymtab_command->nmodtab * sizeof(struct dylib_module_64);

			uint32_t extrefsymoff = dysymtab_command->extrefsymoff;
			uint32_t extrefsize = dysymtab_command->nextrefsyms * sizeof(struct dylib_reference);

			uint32_t indirectsymoff = dysymtab_command->indirectsymoff;
			uint32_t indirectsize = dysymtab_command->nindirectsyms * sizeof(uint32_t);

			uint32_t extreloff = dysymtab_command->extreloff;
			uint32_t extrelsize = dysymtab_command->nextrel * sizeof(struct relocation_info);

			uint32_t locreloff = dysymtab_command->locreloff;
			uint32_t locrelsize = dysymtab_command->nlocrel * sizeof(struct relocation_info);

			dysymtab_command->tocoff = (tocoff - linkedit_old_off) + linkedit_new_off;
			dysymtab_command->modtaboff = (modtaboff - linkedit_old_off) + linkedit_new_off;
			dysymtab_command->extrefsymoff = (extrefsymoff - linkedit_old_off) + linkedit_new_off;
			dysymtab_command->indirectsymoff= (indirectsymoff - linkedit_old_off) + linkedit_new_off;
			dysymtab_command->extreloff = (extreloff - linkedit_old_off) + linkedit_new_off;
			dysymtab_command->locreloff = (locreloff - linkedit_old_off) + linkedit_new_off;

			linkedit_new_sz += (tocsize + modtabsize + extrefsize + indirectsize + extrelsize + locrelsize);

		} else if(load_cmd->cmd == LC_DYLD_INFO_ONLY)
		{
			struct dyld_info_command *dyld_info_command = (struct dyld_info_command*) q;

			uint32_t rebase_off = dyld_info_command->rebase_off;
			uint32_t rebase_size = dyld_info_command->rebase_size;

			uint32_t bind_off = dyld_info_command->bind_off;
			uint32_t bind_size = dyld_info_command->bind_size;

			uint32_t weak_bind_off = dyld_info_command->weak_bind_off;
			uint32_t weak_bind_size = dyld_info_command->weak_bind_size;

			uint32_t lazy_bind_off = dyld_info_command->lazy_bind_off;
			uint32_t lazy_bind_size = dyld_info_command->lazy_bind_size;

			uint32_t export_off = dyld_info_command->export_off;
			uint32_t export_size = dyld_info_command->export_size;

			dyld_info_command->rebase_off = (rebase_off - linkedit_old_off) + linkedit_new_off;
			dyld_info_command->bind_off = (bind_off - linkedit_old_off) + linkedit_new_off;
			dyld_info_command->weak_bind_off = (weak_bind_off - linkedit_old_off) + linkedit_new_off;
			dyld_info_command->lazy_bind_off = (lazy_bind_off - linkedit_old_off) + linkedit_new_off;
			dyld_info_command->export_off = (export_off - linkedit_old_off) + linkedit_new_off;

			linkedit_new_sz += (rebase_size + bind_size + weak_bind_size + lazy_bind_size + export_size);

		} else if(load_cmd->cmd == LC_FUNCTION_STARTS)
		{
			struct linkedit_data_command *linkedit_data_command = (struct linkedit_data_command*) q;

			linkedit_data_command->dataoff = (linkedit_data_command->dataoff - linkedit_old_off) + linkedit_new_off;

			linkedit_new_sz += linkedit_data_command->datasize;

		} else if(load_cmd->cmd == LC_CODE_SIGNATURE)
		{
			struct linkedit_data_command *linkedit_data_command = (struct linkedit_data_command*) q;

			linkedit_data_command->dataoff = (linkedit_data_command->dataoff - linkedit_old_off) + linkedit_new_off;

			linkedit_new_sz += linkedit_data_command->datasize;

		} else if(load_cmd->cmd == LC_DATA_IN_CODE)
		{
			struct linkedit_data_command *linkedit_data_command = (struct linkedit_data_command*) q;

			linkedit_data_command->dataoff = (linkedit_data_command->dataoff - linkedit_old_off) + linkedit_new_off;

			linkedit_new_sz += linkedit_data_command->datasize;
		}

		q = q + load_cmd->cmdsize;
	}

	linkedit_new_sz = (linkedit_new_sz % 0x1000 > 0 ? linkedit_new_sz / 0x1000 + 1 : linkedit_new_sz / 0x1000) * 0x1000;

	return linkedit_new_sz;

exit:
	if(hdr) free(hdr);

	if(cmds) free(cmds);

	return 0;
}

void Dyld::rebuildSymtabStrtab(mach_vm_address_t symtab_, mach_vm_address_t strtab_, mach_vm_address_t linkedit, struct symtab_command *symtab_command)
{
	struct nlist_64 *symtab;

	char *strtab;

	mach_vm_address_t symoff;
	mach_vm_address_t stroff;
	
	size_t symsize;
	size_t strsize;

	int idx = 0;

	symoff = symtab_command->symoff;
	stroff = symtab_command->stroff;

	symsize = symtab_command->nsyms * sizeof(struct nlist_64);
	strsize = symtab_command->strsize;

	symtab = reinterpret_cast<struct nlist_64*>(symtab_);
	strtab = reinterpret_cast<char*>(strtab_);

	for(int i = 0; i < symtab_command->nsyms; i++)
	{
		struct nlist_64* nl = &symtab[i];

		char *sym = this->task->readString(linkedit + stroff + nl->n_strx);

		memcpy(strtab + idx, sym, strlen(sym));

		strtab[idx + strlen(sym)] = '\0';

		nl->n_strx = idx;

		idx += strlen(sym) + 1;

		free(sym);
	}
}

size_t Dyld::getImageSize(mach_vm_address_t address)
{
	bool ok;

	struct mach_header_64 *hdr;

	mach_vm_address_t align;

	uint64_t ncmds;
	size_t sizeofcmds;

	uint8_t *cmds;
	uint8_t *q;

	size_t filesz = 0;

	hdr = NULL;
	cmds = NULL;

	hdr = reinterpret_cast<struct mach_header_64*>(malloc(sizeof(struct mach_header_64)));

	ok = this->task->read(address, hdr, sizeof(struct mach_header_64));

	assert(ok);

	ncmds = hdr->ncmds;
	sizeofcmds = hdr->sizeofcmds;

	cmds = reinterpret_cast<uint8_t*>(malloc(sizeofcmds));

	ok = this->task->read(address + sizeof(struct mach_header_64), cmds, sizeofcmds);

	assert(ok);

	align = sizeof(struct mach_header_64) + hdr->sizeofcmds;

	filesz = (align % 0x1000 > 0 ? align / 0x1000 + 1 : align / 0x1000) * 0x1000;

	q = cmds;

	for(int i = 0; i < ncmds; i++)
	{
		struct load_command *load_cmd = reinterpret_cast<struct load_command*>(q);

		uint32_t cmdtype = load_cmd->cmd;
		uint32_t cmdsize = load_cmd->cmdsize;

		if(load_cmd->cmd == LC_SEGMENT_64)
		{
			struct segment_command_64 *segment = reinterpret_cast<struct segment_command_64*>(q);

			uint64_t fileoff = segment->fileoff;
			uint64_t filesize = segment->filesize;

			if(this->task->getPid() == getpid())
			{
				filesz = max(filesz, fileoff + filesize);
			} else 
			{
				if(strcmp(segment->segname, "__LINKEDIT") == 0)
				{
					filesize = this->getAdjustedLinkeditSize(address);
				}

				filesz += filesize;
			}
		}

		q = q + load_cmd->cmdsize;
	}

	free(hdr);
	free(cmds);

	if(this->task->getPid() == getpid())
	{
		return filesz;
	}

	align = sizeof(struct mach_header_64) + sizeofcmds;

	return filesz + (align % 0x1000 > 0 ? align / 0x1000 + 1 : align / 0x1000) * 0x1000;

exit:

	if(hdr) free(hdr);

	if(cmds) free(cmds);

	return 0;
}

MachO* Dyld::cacheDumpImage(char *image)
{
	UserMachO *macho;

	struct mach_header_64 *hdr;

	uint8_t *cmds;

	mach_vm_address_t symtab;
	mach_vm_address_t strtab;

	mach_vm_address_t linkedit_vmaddr;
	mach_vm_address_t linkedit_off;

	mach_vm_address_t linkedit_old_off;
	mach_vm_address_t linkedit_new_off;

	size_t linkedit_new_sz;

	char *image_dump;

	size_t image_size;

	mach_vm_address_t address = this->getImageLoadedAt(image, NULL);

	printf("address = 0x%llx\n", address);

	size_t size = this->getImageSize(address);

	off_t slide = this->getImageSlide(address);

	printf("address = 0x%llx size = %zx offset = 0x%llx\n", address, size, slide);

	if(size)
	{
		uint8_t *q;

		uint64_t align;

		uint64_t current_offset = 0;

		image_size = this->getImageSize(address);

		image_dump = reinterpret_cast<char*>(malloc(image_size));

		memset(image_dump, 0x0, size);

		printf("Dumping image at 0x%llx with size 0x%zx\n", (uint64_t) image, size);

		this->task->read(address, image_dump, sizeof(struct mach_header_64));

		hdr = reinterpret_cast<struct mach_header_64*>(image);

		this->task->read(address + sizeof(struct mach_header_64), image_dump + sizeof(struct mach_header_64), hdr->sizeofcmds);

		align = sizeof(struct mach_header_64) + hdr->sizeofcmds;

		current_offset = (align % 0x1000 > 0 ? align / 0x1000 + 1 : align / 0x1000) * 0x1000;

		cmds = reinterpret_cast<uint8_t*>(hdr)+ sizeof(struct mach_header_64);

		q = cmds;

		for(int i = 0; i < hdr->ncmds; i++)
		{
			struct load_command *load_cmd = reinterpret_cast<struct load_command*>(q);

			uint32_t cmdtype = load_cmd->cmd;
			uint32_t cmdsize = load_cmd->cmdsize;

			if(load_cmd->cmd == LC_SEGMENT_64)
			{
				struct segment_command_64 *segment = reinterpret_cast<struct segment_command_64*>(q);

				uint64_t vmaddr = segment->vmaddr;
				uint64_t vmsize = segment->vmsize;

				uint64_t fileoffset = segment->fileoff;
				uint64_t filesize = segment->filesize;

				if(strcmp(segment->segname, "__LINKEDIT") == 0)
				{
					linkedit_new_off = current_offset;
					linkedit_old_off = segment->fileoff;

					linkedit_new_sz = this->getAdjustedLinkeditSize(address);

					filesize = linkedit_new_sz;

					linkedit_off = 0;
					linkedit_vmaddr = vmaddr;

					segment->fileoff = current_offset;
					segment->filesize = linkedit_new_sz;

					segment->vmsize = linkedit_new_sz;
				} else 
				{
					segment->fileoff = current_offset;
					segment->filesize = filesize;

					printf("Dumping %s at 0x%llx with size 0x%llx at 0x%llx\n", segment->segname, vmaddr + slide, filesize, (uint64_t) (image_dump + current_offset));

					if(!this->task->read(vmaddr + slide, image_dump + current_offset, filesize))
					{
						printf("Failed to dump segment %s\n", segment->segname);

						goto exit;
					}

					for(int j = 0; j < segment->nsects; j++)
					{
						struct section_64 *section = reinterpret_cast<struct section_64*>(q + sizeof(struct segment_command_64) + sizeof(struct section_64) * j);
						
						if(section->size && section->offset)
						{
							section->offset = current_offset + (section->offset - fileoffset); 
						}
					}

					current_offset += filesize;
				}

			} else if(load_cmd->cmd == LC_SYMTAB)
			{
				struct symtab_command *symtab_command = reinterpret_cast<struct symtab_command*>(q);

				uint64_t symsize = symtab_command->nsyms * sizeof(struct nlist_64);
				uint64_t strsize = symtab_command->strsize;

				uint64_t old_symoff = symtab_command->symoff;
				uint64_t old_stroff = symtab_command->stroff;

				uint64_t symoff = old_symoff - linkedit_new_off;
				uint64_t stroff = old_stroff - linkedit_new_off;

				uint32_t new_strsize = this->getAdjustedStrtabSize(linkedit_old_off + this->dyld_shared_cache, linkedit_old_off, symtab_command);

				// symtab_command->symoff = symoff;
				// symtab_command->stroff = stroff;

				// symtab_command->symoff = old_symoff;
				// symtab_command->stroff = old_stroff;
				
				uint64_t symtab = (uint64_t) (image_dump + linkedit_new_off + linkedit_off);
				uint64_t strtab = (uint64_t) (image_dump + linkedit_new_off + linkedit_off + symsize);

				// symtab_command->symoff = symoff;
				// symtab_command->stroff = stroff;

				// this->task->read(linkedit_vmaddr + slide + symoff, image_dump + linkedit_new_off + linkedit_off, symsize);

				// this->rebuildSymtabStrtab(symtab, strtab, linkedit_vmaddr + slide, symtab_command);

				// symtab_command->symoff = linkedit_new_off + linkedit_off;
				// symtab_command->stroff = linkedit_new_off + linkedit_off + symsize;

				symtab_command->strsize = new_strsize;

				linkedit_off += (symsize + new_strsize);

			} else if(load_cmd->cmd == LC_DYSYMTAB)
			{
				struct dysymtab_command *dysymtab_command = reinterpret_cast<struct dysymtab_command*>(q);

				uint32_t tocoff = dysymtab_command->tocoff - linkedit_new_off;
				uint32_t tocsize = dysymtab_command->ntoc * sizeof(struct dylib_table_of_contents);

				uint32_t modtaboff = dysymtab_command->modtaboff - linkedit_new_off;
				uint32_t modtabsize = dysymtab_command->nmodtab * sizeof(struct dylib_module_64);

				uint32_t extrefsymoff = dysymtab_command->extrefsymoff - linkedit_new_off;
				uint32_t extrefsize = dysymtab_command->nextrefsyms * sizeof(struct dylib_reference);

				uint32_t indirectsymoff = dysymtab_command->indirectsymoff - linkedit_new_off;
				uint32_t indirectsize = dysymtab_command->nindirectsyms * sizeof(uint32_t);

				uint32_t extreloff = dysymtab_command->extreloff - linkedit_new_off;
				uint32_t extrelsize = dysymtab_command->nextrel * sizeof(struct relocation_info);

				uint32_t locreloff = dysymtab_command->locreloff - linkedit_new_off;
				uint32_t locrelsize = dysymtab_command->nlocrel * sizeof(struct relocation_info);

				this->task->read(linkedit_vmaddr + slide + tocoff, image_dump + linkedit_new_off + linkedit_off, tocsize);

				dysymtab_command->tocoff = linkedit_new_off + linkedit_off;

				linkedit_off += tocsize;

				this->task->read(linkedit_vmaddr + slide + modtaboff, image_dump + linkedit_new_off + linkedit_off, modtabsize);

				dysymtab_command->modtaboff = linkedit_new_off + linkedit_off;

				linkedit_off += modtabsize;

				this->task->read(linkedit_vmaddr + slide + extrefsymoff, image_dump + linkedit_new_off + linkedit_off, extrefsize);

				dysymtab_command->extrefsymoff = linkedit_new_off + linkedit_off;

				linkedit_off += extrefsize;

				this->task->read(linkedit_vmaddr + slide + indirectsymoff, image_dump + linkedit_new_off + linkedit_off, indirectsize);

				dysymtab_command->indirectsymoff = linkedit_new_off + linkedit_off;

				linkedit_off += indirectsize;

				this->task->read(linkedit_vmaddr + slide + extreloff, image_dump + linkedit_new_off + linkedit_off, extrelsize);

				dysymtab_command->extreloff = linkedit_new_off + linkedit_off;

				linkedit_off += extrelsize;

				this->task->read(linkedit_vmaddr + slide + locreloff, image_dump + linkedit_new_off + linkedit_off, locrelsize);

				dysymtab_command->locreloff = linkedit_new_off + linkedit_off;

				linkedit_off += locrelsize;

			} else if(load_cmd->cmd == LC_DYLD_INFO_ONLY)
			{
				struct dyld_info_command *dyld_info_command = reinterpret_cast<struct dyld_info_command*>(q);

				uint32_t rebase_off = dyld_info_command->rebase_off - linkedit_new_off;
				uint32_t rebase_size = dyld_info_command->rebase_size;

				uint32_t bind_off = dyld_info_command->bind_off - linkedit_new_off;;
				uint32_t bind_size = dyld_info_command->bind_size;

				uint32_t weak_bind_off = dyld_info_command->weak_bind_off - linkedit_new_off;;
				uint32_t weak_bind_size = dyld_info_command->weak_bind_size;

				uint32_t lazy_bind_off = dyld_info_command->lazy_bind_off - linkedit_new_off;;
				uint32_t lazy_bind_size = dyld_info_command->lazy_bind_size;

				uint32_t export_off = dyld_info_command->export_off - linkedit_new_off;;
				uint32_t export_size = dyld_info_command->export_size;

				this->task->read(linkedit_vmaddr + slide + rebase_off, image_dump + linkedit_new_off + linkedit_off, rebase_size);

				dyld_info_command->rebase_off = linkedit_new_off + linkedit_off;

				linkedit_off += rebase_size;

				this->task->read(linkedit_vmaddr + slide + bind_off, image_dump + linkedit_new_off + linkedit_off, bind_size);

				dyld_info_command->bind_off = linkedit_new_off + linkedit_off;

				linkedit_off += bind_size;

				this->task->read(linkedit_vmaddr + slide + weak_bind_off, image_dump + linkedit_new_off + linkedit_off, weak_bind_size);

				dyld_info_command->weak_bind_off = linkedit_new_off + linkedit_off;

				linkedit_off += weak_bind_size;

				this->task->read(linkedit_vmaddr + slide + lazy_bind_off, image_dump + linkedit_new_off + linkedit_off, lazy_bind_size);

				dyld_info_command->lazy_bind_off = linkedit_new_off + linkedit_off;

				linkedit_off += lazy_bind_size;

				this->task->read(linkedit_vmaddr + slide + export_off, image_dump + linkedit_new_off + linkedit_off, export_size);

				dyld_info_command->export_off = linkedit_new_off + linkedit_off;

				linkedit_off += export_size;

			
			} else if(load_cmd->cmd == LC_FUNCTION_STARTS)
			{
				struct linkedit_data_command *linkedit_data_command = reinterpret_cast<struct linkedit_data_command*>(q);

				uint32_t dataoff = linkedit_data_command->dataoff - linkedit_new_off;
				uint32_t datasize = linkedit_data_command->datasize;

				this->task->read(linkedit_vmaddr + slide + dataoff, image_dump + linkedit_new_off + linkedit_off, datasize);

				linkedit_data_command->dataoff = linkedit_new_off + linkedit_off;

				linkedit_off += datasize;

			} else if(load_cmd->cmd == LC_CODE_SIGNATURE)
			{
				struct linkedit_data_command *linkedit_data_command = reinterpret_cast<struct linkedit_data_command*>(q);

				uint32_t dataoff = linkedit_data_command->dataoff - linkedit_new_off;
				uint32_t datasize = linkedit_data_command->datasize;

				this->task->read(linkedit_vmaddr + slide + dataoff, image_dump + linkedit_new_off + linkedit_off, datasize);

				linkedit_data_command->dataoff = linkedit_new_off + linkedit_off;

				linkedit_off += datasize;

			} else if(load_cmd->cmd == LC_DATA_IN_CODE)
			{
				struct linkedit_data_command *linkedit_data_command = reinterpret_cast<struct linkedit_data_command*>(q);

				uint32_t dataoff = linkedit_data_command->dataoff - linkedit_new_off;
				uint32_t datasize = linkedit_data_command->datasize;

				this->task->read(linkedit_vmaddr + slide + dataoff, image_dump + linkedit_new_off + linkedit_off, datasize);

				linkedit_data_command->dataoff = linkedit_new_off + linkedit_off;

				linkedit_off += datasize;
			}

			q = q + load_cmd->cmdsize;
		}
	}

	macho = new UserMachO();

	macho->initWithBuffer(image);

	return reinterpret_cast<MachO*>(macho);

exit:

	if(image)
	{
		free(image);
	}

	return NULL;
}

MachO* Dyld::cacheDumpImageToFile(char *image, char *path)
{
	FILE *out;

	MachO *macho = this->cacheDumpImage(image);

	if(!macho)
		return NULL;

	out = fopen(path, "w");

	if(!out)
		return NULL;

	fclose(out);

	return macho;
}