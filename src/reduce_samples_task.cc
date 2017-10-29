/*
 * Copyright (c) 2017 Tuukka Norri
 * This code is licensed under MIT license (see LICENSE for details).
 */

#include <boost/io/ios_state.hpp>
#include <vcf2multialign/file_handling.hh>
#include <vcf2multialign/tasks/read_subgraph_variants_task.hh>
#include <vcf2multialign/tasks/reduce_samples_task.hh>


namespace vcf2multialign {
	
	void reduce_samples_task::init_read_subgraph_variants_task(
		read_subgraph_variants_task &task,
		std::size_t const task_idx,
		char const *buffer_start,
		char const *buffer_end,
		std::size_t const start_lineno,
		std::size_t const variant_count
	)
	{
		auto const qname(boost::str(boost::format("fi.iki.tsnorri.vcf2multialign.worker-queue-%u") % task_idx));
		dispatch_ptr <dispatch_queue_t> worker_queue(
			dispatch_queue_create(qname.c_str(), DISPATCH_QUEUE_SERIAL),
			false
		);
			
		read_subgraph_variants_task temp_task(
			*this,
			worker_queue,
			*m_status_logger,
			*m_error_logger,
			m_reader,
			*m_alt_checker,
			*m_reference,
			m_sv_handling_method,
			*m_skipped_variants,
			*m_out_reference_fname,
			task_idx,
			m_generated_path_count,
			m_sample_ploidy_sum,
			start_lineno,
			variant_count
		);
		
		// Make the task parse the given range.
		auto &task_reader(temp_task.vcf_reader());
		task_reader.set_buffer_start(buffer_start);
		task_reader.set_buffer_end(buffer_end);
		task_reader.set_eof(buffer_end);
		
		task = std::move(temp_task);
	}
	
	
	void reduce_samples_task::start_merge_task(std::size_t const lhs_idx, std::size_t const rhs_idx)
	{
		auto const &lhs(m_subgraphs[lhs_idx]);
		auto const &rhs(m_subgraphs[rhs_idx]);
		std::unique_ptr <task> task(new merge_subgraph_paths_task(*this, m_semaphore, lhs, rhs, lhs_idx, m_generated_path_count));
		m_delegate->store_and_execute(std::move(task));
	}
	
	
	auto reduce_samples_task::create_haplotype(
		std::size_t const sample_no,
		std::size_t const ploidy
	) -> haplotype_map_type::iterator
	{
		return m_haplotypes.emplace(
			std::piecewise_construct,
			std::forward_as_tuple(sample_no),
			std::forward_as_tuple(ploidy)
		).first;
	}
	
	
	void reduce_samples_task::prepare_haplotypes()
	{
		// FIXME: partially duplicate code with all_haplotypes_task.
		// FIXME: m_generated_path_count (plus some small value) may not exceed the maximum number of open files.
		for (std::size_t i(0); i < m_generated_path_count; ++i)
		{
			auto const fname(boost::str(boost::format("%u") % (1 + i)));
			auto it(create_haplotype(1 + i, 1));
			auto &haplotype_vec(it->second);
			open_file_channel_for_writing(
				fname.c_str(),
				haplotype_vec[0].output_stream,
				m_write_semaphore,
				m_should_overwrite_files
			);
		}
		
		if (m_out_reference_fname->operator bool())
		{
			always_assert(
				m_haplotypes.cend() == m_haplotypes.find(REF_SAMPLE_NUMBER),
				"REF_SAMPLE_NUMBER already in use"
			);
			
			auto it(create_haplotype(REF_SAMPLE_NUMBER, 1));
			auto &haplotype_vec(it->second);
			open_file_channel_for_writing(
				(*m_out_reference_fname)->c_str(),
				haplotype_vec[0].output_stream,
				m_write_semaphore,
				m_should_overwrite_files
			);
		}
	}
	
	
	void reduce_samples_task::task_did_finish(
		merge_subgraph_paths_task &task,
		std::vector <reduced_subgraph::path_index> &&matchings
	)
	{
		// As per [container.requirements.dataraces] this should be thread-safe since different
		// threads may not modify the same element.
		auto const idx(task.left_subgraph_index());
		m_path_matchings[idx] = std::move(matchings);
		
		// According to CppReference, the above should become a visible side effect of m_remaining_merge_tasks.load()
		// (a consequence of calling m_remaining_merge_tasks.operator--() and the default memory order being 
		// std::memory_order_seq_cst):
		// “–– everything that happened-before a store in one thread becomes a visible
		// side effect in the thread that did a load –– ”
		if (0 == --m_remaining_merge_tasks)
		{
			auto task(new sequence_writer_task(
				*this,
				*m_status_logger,
				*m_error_logger,
				m_reader,
				*m_alt_checker,
				*m_reference,
				*m_null_allele_seq
			));
				
			std::unique_ptr <class task> task_ptr(task);
			
			prepare_haplotypes();
			task->prepare(m_haplotypes);
			
			m_subgraph_iterator = m_subgraphs.cbegin();
			
			m_delegate->store_and_execute(std::move(task_ptr));
		}
	}
	
	
	void reduce_samples_task::task_did_finish(sequence_writer_task &task)
	{
		// Wait with a dispatch group that every file has finished writing.
		dispatch_queue_t queue(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
		dispatch_ptr <dispatch_group_t> group(dispatch_group_create());
		for (auto &kv : m_haplotypes)
		{
			// Actually there's only one element in each kv.second.
			for (auto &haplotype : kv.second)
				haplotype.output_stream->close_async(*group);
		}
		
		dispatch_group_notify_fn(*group, queue, [this](){
			m_delegate->task_did_finish(*this);
		});
	}
	
	
	void reduce_samples_task::enumerate_sample_genotypes(
		variant const &var,
		std::function <void(std::size_t, uint8_t, uint8_t, bool)> const &cb	// sample_no, chr_idx, alt_idx, is_phased
	)
	{
		auto const var_lineno(var.lineno());
		auto const pos_in_subgraph(var.variant_index());
		
		// Find the correct subgraph.
		// Update m_path_permutation if the next subgraph is chosen.
		while (true)
		{
			always_assert(m_subgraphs.cend() != m_subgraph_iterator);
			
			auto const &subgraph(*m_subgraph_iterator);
			auto const subgraph_start_lineno(subgraph.start_lineno());
			auto const subgraph_var_count(subgraph.variant_count());
			
			always_assert(subgraph_start_lineno <= var_lineno);
			if (var_lineno < subgraph_start_lineno + subgraph_var_count)
				break;
			
			// Update m_path_permutation.
			auto const subgraph_idx(m_subgraph_iterator - m_subgraphs.cbegin());
			auto const &matching(m_path_matchings[subgraph_idx]);
			auto const previous_permutation(m_path_permutation); // Copy.
			
			std::size_t i(0);
			for (auto const idx : previous_permutation)
			{
				m_path_permutation[i] = matching[idx];
				++i;
			}
			
			++m_subgraph_iterator;
		}
		
		// Enumerate the genotypes in the permutation order.
		std::size_t sample_no(0);
		static_assert(0 == REF_SAMPLE_NUMBER);
		cb(REF_SAMPLE_NUMBER, 0, 0, true);	// REF.
		for (auto const path_idx : m_path_permutation)
		{
			auto const &sequence(m_subgraph_iterator->path_sequence(path_idx));
			auto const alt_idx(sequence[pos_in_subgraph]);
			
			// REF is zero, see the assertion above.
			cb(++sample_no, 0, alt_idx, true);
		}
	}
	
	
	void reduce_samples_task::task_did_finish(read_subgraph_variants_task &task, reduced_subgraph &&rsg)
	{
		// Store the reduced subgraph. Use the mutex to make this thread-safe.
		std::lock_guard <std::mutex> lock_guard(m_subgraph_mutex);
		// Tasks are numbered in subgraph starting point order.
		auto const subgraph_idx(task.task_idx());
		m_subgraphs[subgraph_idx] = std::move(rsg);
		m_subgraph_bitmap[subgraph_idx] = true;
		
		// Start merging tasks if possible.
		// Currently start_merge_task requires the lock above.
		if (subgraph_idx && m_subgraph_bitmap[subgraph_idx - 1])
			start_merge_task(subgraph_idx - 1, subgraph_idx);
		
		auto const last_idx(m_subgraph_bitmap.size() - 1);
		if (subgraph_idx < last_idx && m_subgraph_bitmap[last_idx])
			start_merge_task(subgraph_idx, subgraph_idx + 1);
	}
	
	
	void reduce_samples_task::execute()
	{
		struct graph_range
		{
			char const 	*subgraph_start{nullptr};
			char const 	*subgraph_end{nullptr};
			std::size_t start_lineno{0};
			std::size_t	variant_count{0};
			
			graph_range() = default;
			graph_range(
				char const 	*subgraph_start_,
				char const 	*subgraph_end_,
				std::size_t start_lineno_,
				std::size_t	variant_count_
			):
				subgraph_start(subgraph_start_),
				subgraph_end(subgraph_end_),
				start_lineno(start_lineno_),
				variant_count(variant_count_)
			{
			}
		};
		
		std::vector <graph_range> graph_ranges;
		
		// Create a task for each sufficiently long range.
		{
			auto const buffer_start(m_reader.buffer_start());
			auto const buffer_end(m_reader.buffer_end());
			auto current_start(buffer_start);
			std::size_t current_lineno(1 + m_reader.last_header_lineno());
		
			for (auto const &kv : *m_subgraph_starting_points)
			{
				auto const subgraph_start(kv.first);
				auto const var_lineno(kv.second);
			
				if (m_min_path_length <= subgraph_start - current_start)
					graph_ranges.emplace_back(current_start, subgraph_start, current_lineno, var_lineno - current_lineno);
			}
		
			// Add the final graph_range to cover the remaining range.
			graph_ranges.emplace_back(current_start, buffer_end, current_lineno, m_record_count - current_lineno);
		}
		
		// Allocate enough space for subgraphs.
		auto const range_count(graph_ranges.size());
		always_assert(range_count);
		m_subgraphs.resize(range_count);
		m_subgraph_bitmap.resize(range_count);
		m_path_matchings.resize(range_count - 1);
		m_remaining_merge_tasks = range_count - 1;
		
		// Start each task.
		std::size_t task_idx(0);
		for (auto const &range : graph_ranges)
		{
			auto *task(new read_subgraph_variants_task);
			std::unique_ptr <class task> task_ptr(task);
			init_read_subgraph_variants_task(
				*task,
				task_idx++,
				range.subgraph_start,
				range.subgraph_end,
				range.start_lineno,
				range.variant_count
			);
			m_delegate->store_and_execute(std::move(task_ptr));
		}
	}
}