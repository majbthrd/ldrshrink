/*
    command-line tool to simplify a ADSP-SC58x/BF70x loader file so as to boot faster
    Copyright (C) 2015,2016,2018 Peter Lawrence

    This was written to hopefully encourage Analog Devices to fix
    limitations in their "elfloader" utility and ADSP-SC58x/-BF70x Boot ROM.

    Failing that, it might aid engineers trying to optimize boot time.

    At the time of writing, the "elfloader" utility fails to merge contiguous 
    sections into single blocks.  The ADSP-SC58x and -BF70x Boot ROM, in turn, 
    is inefficient in execution time between blocks, meaning that the 
    inefficiency of "elfloader" can cost dearly in boot times.

    This same Boot ROM inefficiency can be seen in small Fill blocks.  The 
    objective of "elfloader" is to run-length-encode any repeating sequences 
    of words to achieve "space compression", but the added execution time 
    due to these smaller blocks overwhelms any small size reductions.

    Permission is hereby granted, free of charge, to any person obtaining a 
    copy of this software and associated documentation files (the "Software"), 
    to deal in the Software without restriction, including without limitation 
    the rights to use, copy, modify, merge, publish, distribute, sublicense, 
    and/or sell copies of the Software, and to permit persons to whom the 
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in 
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
    DEALINGS IN THE SOFTWARE.
*/

/*
    20151124 : initial release to Analog Devices (who opened issue CCES-14431)
    20160803 : release of code to github
    20181115 : unroll small FILL blocks (threshold in CUSTOMIZE_SMALLEST_FILL_BLOCK)
*/

#include <stdio.h>
#include <malloc.h>
#include <string.h>

/*
abbreviated and combined information from ADSP-SC58x and ADSP-BF70x Hardware Reference Manuals
*/
#define BFLAG_FILL      0x010 /* Fill the target location with a specified 32-bit value. */
#define BFLAG_INIT      0x080 /* Calls function at target address after loading payload to the same address. */
#define BFLAG_IGNORE    0x100 /* Block payload is ignored. */
#define BFLAG_FIRST     0x400 /* Indicates the block to be the beginning of a new application */
#define BFLAG_FINAL     0x800 /* Indicates the last block of a loader stream. Booting will complete after processing the block. This flag does not denote the end of an application in a Multi-Application Boot Streams boot stream */

struct block_header_type
{
	struct block_code_bitfield
	{
		unsigned bcode:4;
		unsigned flags:12;
		unsigned hdrchk:8;
		unsigned hdrsign:8;
	} block_code;
	unsigned target_address;
	unsigned byte_count;
	unsigned argument;
};

struct chunk_list_type
{
	unsigned address;
	unsigned argument;
	unsigned char *data;
	unsigned length;
	unsigned flags;
	struct chunk_list_type *next;
};

struct image_settings_type
{
	unsigned char hdrsign, bcode;
	unsigned entry_point;
};

static void write_header(FILE *handle, struct block_header_type *hdr);
static unsigned char calc_header_checksum(struct block_header_type *hdr);
static void write_image(FILE *handle, struct chunk_list_type *list, struct image_settings_type *settings);
static void copy_settings(struct image_settings_type *settings, struct block_header_type *hdr);
static void print_flags(unsigned flags, unsigned arguments);

#define CUSTOMIZE_SMALLEST_FILL_BLOCK 256

int main(int argc, char *argv[])
{
	FILE *input, *output;
	struct block_header_type hdr;
	unsigned position;
	unsigned char checksum;
	struct chunk_list_type *list = NULL;
	struct chunk_list_type *current, *previous, *additional;
	unsigned additional_bytes;
	struct image_settings_type settings;
	unsigned input_block_count, output_block_count;
	unsigned char *ptr;

	if (argc < 3)
	{
		fprintf(stderr, "%s <input_ldr> <output_ldr>\n", argv[0]);
		return -1;
	}

	input = fopen(argv[1], "rb");

	if (NULL == input)
	{
		fprintf(stderr, "ERROR: unable to open input file\n");
		return -1;
	}

	output = fopen(argv[2], "wb");

	if (NULL == output)
	{
		fprintf(stderr, "ERROR: unable to open output file\n");
		return -1;
	}

	position = 0;
	input_block_count = output_block_count = 0;

	while (fread(&hdr, sizeof(struct block_header_type), 1, input))
	{
		/* compute the XOR checksum */
		checksum = calc_header_checksum(&hdr);

		/* stop execution if the checksum failed; the checksum passes only when it calculates to zero */
		if (checksum)
		{
			printf("ERROR: checksum failed @ 0x%02x\n", position);
			return -1;
		}

		/* keep track of position (and print for diagnostic purposes) */
		position += sizeof(struct block_header_type);
		if (!(hdr.block_code.flags & (BFLAG_FIRST | BFLAG_FINAL)))
		{
			printf("0x%x 0x%x", hdr.target_address, hdr.byte_count);
			print_flags(hdr.block_code.flags, hdr.argument);
		}

		if (hdr.block_code.flags & (BFLAG_FIRST | BFLAG_FINAL))
			if (list)
			{
				write_image(output, list, &settings);
				list = NULL;
			}

		/* bail while loop if we've reached the end */
		if (hdr.block_code.flags & BFLAG_FINAL)
		{
			break;
		}

		/* if this is a First Block, we note the entry point address and immediately loop again to read the next block */
		if (hdr.block_code.flags & BFLAG_FIRST)
		{
			settings.entry_point = hdr.target_address;
			settings.hdrsign = hdr.block_code.hdrsign;
			settings.bcode = hdr.block_code.bcode;
			
			printf("--- read 0x%02x entry 0x%x\n", hdr.block_code.hdrsign, hdr.target_address);
			continue;
		}

		input_block_count++;

		/* if this is an Ignore Block (other than a First Block), we throw away the data */
		if (hdr.block_code.flags & BFLAG_IGNORE)
		{
			fseek(input, hdr.byte_count, SEEK_CUR);
			continue;
		}

		/* keep track of position for diagnostic purposes */
		if (!(hdr.block_code.flags & BFLAG_FILL))
			position += hdr.byte_count;

		/* find the right place to insert the data into the linked list */

		current = list;
		previous = NULL;
		additional = NULL;

		while (current)
		{
			if (hdr.block_code.flags & ~BFLAG_FILL)
				goto not_a_match; /* skip check if any bit other than BFLAG_FILL is set; we'll just arrive at the end of the linked list */

			if ( (hdr.block_code.flags & BFLAG_FILL) && (hdr.byte_count > CUSTOMIZE_SMALLEST_FILL_BLOCK) )
				goto not_a_match; /* this FILL block is too large to justify unrolling; we'll just arrive at the end of the linked list  */

			if (current->flags & BFLAG_FILL)
				goto not_a_match; /* segregate: do not join to existing FILL blocks */

			if ( ( hdr.target_address >= current->address ) && ( hdr.target_address <= (current->address + current->length)) )
			{
				/* this block is contiguous with an already seen block */
				additional = current;
				break;
			}

not_a_match:
			previous = current;
			current = current->next;
		}

		/* determine whether we need to add an additional entry */

		if (!additional)
		{
			/* an additional entry is needed, so we create it and add it to the list */

			additional = (struct chunk_list_type *)malloc(sizeof(struct chunk_list_type));
			memset(additional, 0, sizeof(struct chunk_list_type));
			additional->address = hdr.target_address;
			additional->argument = hdr.argument;
			additional->flags = hdr.block_code.flags;

			if (previous)
				previous->next = additional;
			else
				list = additional;

			additional->next = current;
			output_block_count++;
		}

		/* compute how many bytes are being added to this entry (whether the entry is additional or existing) */

		if ( (hdr.target_address + hdr.byte_count) > (additional->address + additional->length) )
		{
			additional_bytes = (hdr.target_address + hdr.byte_count) - (additional->address + additional->length);

			if (hdr.block_code.flags & BFLAG_FILL)
			{
				/* this is a Fill Block... we append if and only if the current Block isn't also a Fill Block */
				if (!(additional->flags & BFLAG_FILL))
				{
					additional->data = realloc(additional->data, additional->length + additional_bytes);
					ptr = additional->data + (hdr.target_address - additional->address);
					while (hdr.byte_count >= sizeof(hdr.argument))
					{
						memcpy(ptr, &hdr.argument, sizeof(hdr.argument));
						ptr += sizeof(hdr.argument);
						hdr.byte_count -= sizeof(hdr.argument);
					}
				}
			}
			else
			{
				/* this is not a Fill Block, so we read in the data */
				additional->data = realloc(additional->data, additional->length + additional_bytes);
				fread(additional->data + (hdr.target_address - additional->address), 1, additional_bytes, input);
			}

			/* update the entry length to reflect the added data */
			additional->length += additional_bytes;
		}
		else
		{
			if (hdr.byte_count)
				printf("WARNING: memory overwrite in region 0x%x to 0x%x\n", hdr.target_address, hdr.target_address + hdr.byte_count);
		}

		if (hdr.block_code.flags & BFLAG_INIT)
		{
			if (list)
			{
				write_image(output, list, &settings);
				list = NULL;
			}
		}
	}

	fclose(input);

	/* finish the output file with the Final Block */

	hdr.block_code.flags = BFLAG_FINAL;
	hdr.target_address = settings.entry_point;
	hdr.argument = 0;
	hdr.byte_count = 0;

	write_header(output, &hdr);

	fclose(output);

	/* provide some metrics on how much the loader image has been simplified */
	printf("---\n%d blocks read; %d blocks written\n", input_block_count, output_block_count);

	return 0;
}

static void write_header(FILE *handle, struct block_header_type *hdr)
{
	/* set checksum to zero so that... */
	hdr->block_code.hdrchk = 0;
	/* ...the calculated result will be the correct checksum */
	hdr->block_code.hdrchk = calc_header_checksum(hdr);

	fwrite(hdr, sizeof(struct block_header_type), 1, handle);
}

static unsigned char calc_header_checksum(struct block_header_type *hdr)
{
	unsigned char checksum;
	unsigned index;

	/* checksum is an XOR of all bytes in the header */
	checksum = 0;
	for (index = 0; index < sizeof(struct block_header_type); index++)
		checksum ^= *((unsigned char *)hdr + index);

	return checksum;
}

static void write_image(FILE *handle, struct chunk_list_type *list, struct image_settings_type *settings)
{
	struct chunk_list_type *current, *previous;
	unsigned position;
	struct block_header_type hdr;

	/*
	for diagnostic purposes, we print out what we've simplified the loader data into
	*/

	printf("--- write 0x%02x entry 0x%x\n", settings->hdrsign, settings->entry_point);

	current = list;
	position = sizeof(struct block_header_type);

	while (current)
	{
		printf("0x%x 0x%x", current->address, current->length);
		print_flags(current->flags, current->argument);

		position += sizeof(struct block_header_type);
		if (current->data)
			position += current->length;

		current = current->next;
	}

	/*
	now we write out the new, simplfied loader image
	*/

	memset(&hdr, 0, sizeof(struct block_header_type));

	hdr.block_code.bcode = settings->bcode;
	hdr.block_code.flags = BFLAG_IGNORE | BFLAG_FIRST;
	hdr.block_code.hdrsign = settings->hdrsign;
	hdr.target_address = settings->entry_point;
	hdr.argument = position;
	
	write_header(handle, &hdr);

	current = list;
	previous = NULL;

	while (current)
	{
		hdr.block_code.flags = current->flags;
		hdr.argument = current->argument;
		hdr.byte_count = current->length;
		hdr.target_address = current->address;

		write_header(handle, &hdr);

		if (current->data)
		{
			/* this is not a Fill Block, so write out the data */
			fwrite(current->data, 1, current->length, handle);
			free(current->data);
		}

		previous = current;
		current = current->next;
		free(previous);
	}
}

static void print_flags(unsigned flags, unsigned argument)
{
	if (flags & BFLAG_FILL)
		printf(" FILL (0x%x)", argument);
	if (flags & BFLAG_INIT)
		printf(" INIT");
	printf("\n");
}
