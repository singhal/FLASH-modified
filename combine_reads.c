#include <stdio.h>

#include "combine_reads.h"
#include "fastq.h"
#include "util.h"


/*
 * Align the read @seq_1 to the read @seq_2, where @overlap_len is the length of
 * the attempted overlap and the length of @seq_1, and @seq_2_len is the length
 * of @seq_2.
 *
 * In the output paramater @mismatch_density_ret, return the density of base
 * pair mismatches in the overlapped region.  In the output parameter
 * @qual_score_ret, return the average of the quality scores of all mismatches
 * in the overlapped region.
 *
 * The maximum overlap length considered in the scoring is @max_overlap, while
 * the minimum is @min_overlap.
 */
static void align_position(const char * restrict seq_1,
			   const char * restrict seq_2,
			   const char * restrict qual_1,
			   const char * restrict qual_2,
			   int overlap_len,
			   int min_overlap,
			   int max_overlap,
			   float *mismatch_density_ret,
			   float *qual_score_ret)
{
	int num_non_dna_chars = 0;
	int num_mismatches = 0;
	int mismatch_qual_total = 0;

	/* Calculate the number of mismatches, the number of 'N' characters, and
	 * the sum of the minimum quality values in the mismatched bases over
	 * the overlap region. */
	for (int i = 0; i < overlap_len; i++) {
		if (seq_1[i] == 'N' || seq_2[i] == 'N') {
			num_non_dna_chars++;
		} else {
			if (seq_1[i] != seq_2[i])  {
				num_mismatches++;
				mismatch_qual_total += min(qual_1[i], qual_2[i]);
			}
		}
	}

	/* Reduce the length of overlap by the number of N's. */
	overlap_len -= num_non_dna_chars;

	/* Set the qual_score and mismatch_density.  qual_score is for
	 * mismatches only, so lower qual_score is preferable since lower
	 * qual_score means less confidence in base calls at mismatches. */
	if (overlap_len > max_overlap) {
		*qual_score_ret = (float)mismatch_qual_total / max_overlap;
		*mismatch_density_ret = (float)num_mismatches / max_overlap;
	} else  {
		if (overlap_len >= min_overlap) {
			*qual_score_ret = (float)mismatch_qual_total / overlap_len;
			*mismatch_density_ret = (float)num_mismatches / overlap_len;
		} else {
			*qual_score_ret = 0.0f;
			*mismatch_density_ret = 10000.0f;
		}
	}
}


/*
 * Return the position of the first overlapping character in the first read
 * (0-based) of the best overlap between two reads, or -1 if the best overlap
 * does not satisfy the required threshold.
 */
static int pair_align(const struct read *read_1, const struct read *read_2,
		      int min_overlap, int max_overlap,
		      float max_mismatch_density)
{
	/* Best (smallest) mismatch density that has been found so far in an
	 * overlap. */
	float best_mismatch_density = max_mismatch_density + 1.0f;
	float best_qual_score = 0.0f;
	int best_position = -1;

	/* Require at least min_overlap bases overlap, and require that the
	 * second read is not overlapped such that it is completely contained in
	 * the first read.  */
	int start = 0;
	int end = read_1->seq_len - min_overlap + 1;
	for (int i = start; i < end; i++) {
		float mismatch_density;
		float qual_score;

		align_position(read_1->seq + i,
			       read_2->seq,
			       read_1->qual + i,
			       read_2->qual,
			       min(read_1->seq_len - i, read_2->seq_len),
			       min_overlap, max_overlap,
			       &mismatch_density, &qual_score);

		if (mismatch_density < best_mismatch_density) {
			best_qual_score       = qual_score;
			best_mismatch_density = mismatch_density;
			best_position         = i;
		} else if (mismatch_density == best_mismatch_density) {
			if (qual_score < best_qual_score) {
				best_qual_score = qual_score;
				best_position = i;
			}
		}
	}

	if (best_mismatch_density > max_mismatch_density)
		return -1;
	else
		return best_position;
}

bool combine_reads(const struct read *read_1, const struct read *read_2,
		   struct read *combined_read, int min_overlap,
		   int max_overlap, float max_mismatch_density)
{
	/* Starting position of the alignment in the first read, 0-based. */
	int overlap_begin;

	/* Length of the overlapping part of two reads. */
	int overlap_len;

	/* Length of the part of the second read not overlapped with the first
	 * read. */
	int remaining_len;

	/* Length of the combined read. */
	int combined_seq_len;

	/* Do the alignment both ways */
	int overlap_begin_1 = pair_align(read_1, read_2, min_overlap, max_overlap,
									 max_mismatch_density);
	int overlap_begin_2 = pair_align(read_2, read_1, min_overlap, max_overlap,
									 max_mismatch_density);

	if (overlap_begin_1 < 0 && overlap_begin_2 < 0)
		return false;
	else if (overlap_begin_1 < 0)
		overlap_begin = overlap_begin_2;
	else if (overlap_begin_2 < 0)
		overlap_begin = overlap_begin_1;
	else {
		float mismatch_density_1;
		float qual_score_1;
		align_position(read_1->seq + overlap_begin_1, read_2->seq,
			       	   read_1->qual + overlap_begin_1, read_2->qual,
			       	   min(read_1->seq_len - overlap_begin_1, read_2->seq_len),
			       	   min_overlap, max_overlap, &mismatch_density_1, &qual_score_1);
		float mismatch_density_2;
		float qual_score_2;
		align_position(read_2->seq + overlap_begin_2, read_1->seq,
					   read_2->qual + overlap_begin_2, read_1->qual,
					   min(read_2->seq_len - overlap_begin_2, read_1->seq_len),
					   min_overlap, max_overlap, &mismatch_density_2, &qual_score_2);

		if (mismatch_density_1 < mismatch_density_2)
			overlap_begin = overlap_begin_1;
		else if (mismatch_density_1 > mismatch_density_2)
			overlap_begin = overlap_begin_2;
		else if (qual_score_1 < qual_score_2)
			overlap_begin = overlap_begin_1;
		else if (qual_score_1 > qual_score_2)
			overlap_begin = overlap_begin_2;
		else
			overlap_begin = overlap_begin_1;
	}

	if (overlap_begin == overlap_begin_1)
		sleep(0); //Do nothing
	else if (overlap_begin == overlap_begin_2) {
		const struct read *tmp = read_1;
		read_1 = read_2;
		read_2 = tmp;
	}


	/* Fill in the combined read. */
	overlap_len = read_1->seq_len - overlap_begin;
	remaining_len = read_2->seq_len - overlap_len;
	remaining_len = (remaining_len < 0 ? 0 : remaining_len);
	combined_seq_len = read_1->seq_len + remaining_len;

	if (combined_read->seq_bufsz < combined_seq_len) {
		combined_read->seq = xrealloc(combined_read->seq, combined_seq_len);
		combined_read->qual = xrealloc(combined_read->qual, combined_seq_len);
		combined_read->seq_bufsz = combined_seq_len;
	}
	combined_read->seq_len = combined_seq_len;


	const char * restrict seq_1 = read_1->seq;
	const char * restrict seq_2 = read_2->seq;
	const char * restrict qual_1 = read_1->qual;
	const char * restrict qual_2 = read_2->qual;
	char * restrict combined_seq = combined_read->seq;
	char * restrict combined_qual = combined_read->qual;


	/* Copy the beginning of the first read. */
	while (overlap_begin--) {
		*combined_seq++ = *seq_1++;
		*combined_qual++ = *qual_1++;
	}

	/* Copy the overlapping part. */
	while (overlap_len--) {
		if (*seq_1 == *seq_2 || *seq_2 == '\0') {
			/* Same base in both reads.  Take the higher quality
			 * value. */
			*combined_seq = *seq_1;
			*combined_qual = max(*qual_1, *qual_2);
		} else {
			/* Different bases in the two reads.  Take the base from
			 * the read that has the higher quality value, but use
			 * the lower quality value, and use a quality value of
			 * at most 2 (+ phred_offset in the final output--- here
			 * the quality values are all scaled to start at 0.) */

			*combined_qual = min(*qual_1, *qual_2);
			*combined_qual = min(*combined_qual, 2);

			if (*qual_1 > *qual_2) {
				*combined_seq = *seq_1;
			} else if (*qual_1 < *qual_2) {
				*combined_seq = *seq_2;
			} else {
				/* Same quality value; take the base from the
				 * first read if the base from the second read
				 * is an 'N'; otherwise take the base from the
				 * second read. */
				if (*seq_2 == 'N')
					*combined_seq = *seq_1;
				else
					*combined_seq = *seq_2;
			}
		}
		combined_seq++;
		combined_qual++;
		seq_1++;
		seq_2++;
		qual_1++;
		qual_2++;
	}
	/* Copy the part of the second read in the mate pair that is not in the
	 * overlapped region. */
	while (remaining_len--) {
		*combined_seq++ = *seq_2++;
		*combined_qual++ = *qual_2++;
	}

	return true;
}
