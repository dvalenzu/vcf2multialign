/*
 * Copyright (c) 2017 Tuukka Norri
 * This code is licensed under MIT license (see LICENSE for details).
 */

#include <vcf2multialign/util.hh>
#include <vcf2multialign/variant_buffer.hh>


namespace vcf2multialign {

	void variant_buffer::return_node_to_buffer(variant_set::node_type &&node)
	{
		std::lock_guard <std::mutex> guard(m_nm.buffer_mutex);
		m_nm.buffer.emplace_back(std::move(node));
	}
	
	
	bool variant_buffer::get_node_from_buffer(variant_set::node_type &node)
	{
		std::lock_guard <std::mutex> guard(m_nm.buffer_mutex);
		auto const bufsize(m_nm.buffer.size());
		if (bufsize)
		{
			using std::swap;
			swap(node, m_nm.buffer[bufsize - 1]);
			m_nm.buffer.pop_back();
			return true;
		}
		return false;
	}
	
	
	void variant_buffer::read_input()
	{
		m_previous_pos = 0;
		
		bool should_continue(false);
		do
		{
			// Read from the stream.
			m_reader->fill_buffer();
			
			should_continue = m_reader->parse([this](transient_variant const &transient_variant) -> bool {
				
				using std::swap;
				
				// Get a node handle.
				variant_set::node_type node;	// Empty, insert does nothing.
				if (! get_node_from_buffer(node))
				{
					// No handles in the buffer, create a new one and retrieve it.
					node = m_factory.extract(m_factory.emplace());
				}
				
				// Copy the variant to node.
				node.value() = transient_variant;
				
				// Check if the variant has a new POS value.
				auto const variant_pos(node.value().pos());
				if (variant_pos != m_previous_pos)
				{
					m_previous_pos = variant_pos;
					variant_set prepared_variants;
					swap(m_prepared_variants, prepared_variants);
		
					auto fn = [this, pr = std::move(prepared_variants)]() mutable {
						process_input(pr);
					};
					dispatch_async_fn(*m_worker_queue, std::move(fn));
					
					// Wait for our turn to continue.
					// Do this only after the worker queue has received something to process so that
					// a long span of variants with the same POS don't cause a deadlock.
					auto const st(dispatch_semaphore_wait(*m_process_sema, DISPATCH_TIME_FOREVER));
					always_assert(0 == st, "dispatch_semaphore_wait returned early");
				}
				
				// Add the node to the input list.
				m_prepared_variants.insert(std::move(node));
				
				return true;
			});
		} while (should_continue);

		if (!m_prepared_variants.empty())
		{
			using std::swap;
			variant_set prepared_variants;
			swap(m_prepared_variants, prepared_variants);
			auto fn = [this, pr = std::move(prepared_variants)]() mutable {
				process_input(pr);
			};
			dispatch_async_fn(*m_worker_queue, std::move(fn));
		}
		
		dispatch(m_delegate).async <&variant_buffer_delegate::finish>(*m_worker_queue);
	}
	
	
	void variant_buffer::process_input(variant_set &variants)
	{
		while (!variants.empty())
		{
			auto node(variants.extract(variants.cbegin()));
			auto &var(node.value());
			
			// Process the input.
			m_delegate->handle_variant(var);
	
			// Return the node.
			return_node_to_buffer(std::move(node));
	
			dispatch_semaphore_signal(*m_process_sema);
		}
	}
}
