/*
 * Copyright (c) 2017 Tuukka Norri
 * This code is licensed under MIT license (see LICENSE for details).
 */

#ifndef VCF2MULTIALIGN_SAMPLE_REDUCER_HH
#define VCF2MULTIALIGN_SAMPLE_REDUCER_HH

#include <boost/container/list.hpp>
#include <boost/container/map.hpp>
#include <boost/container/vector.hpp>
#include <map>
#include <utility>
#include <vcf2multialign/variant.hh>
#include <vcf2multialign/variant_processor_delegate.hh>
#include <vcf2multialign/vcf_reader.hh>
#include <vector>


namespace vcf2multialign {
	
	typedef std::pair <std::size_t, uint8_t> variant_sequence_id;


	class variant_sequence
	{
		friend std::ostream &operator<<(std::ostream &, variant_sequence const &);

	protected:
		variant_sequence_id				m_seq_id{};
		std::size_t						m_start_pos_1{0};	// First ALT position in genome co-ordinates.
		std::size_t						m_end_pos{0};		// Last ALT position plus one in genome co-ordinates.
		std::map <std::size_t, uint8_t>	m_alt_indices{};	// ALT indices by lineno.
	
	public:
		variant_sequence() = default;
	
		variant_sequence(
			std::size_t const sample_no,
			uint8_t const chr_idx
		):
			m_seq_id(sample_no, chr_idx)
		{
		}
	
		variant_sequence(variant_sequence_id const &seq_id):
			m_seq_id(seq_id)
		{
		}
	
		std::size_t start_pos_1() const { return m_start_pos_1; }
		std::size_t start_pos() const { return m_start_pos_1 - 1; }
		std::size_t end_pos() const { return m_end_pos; }
		std::size_t length() const { return 1 + m_end_pos - m_start_pos_1; }
		variant_sequence_id const &seq_id() const { return m_seq_id; }
		std::size_t sample_no() const { return m_seq_id.first; }
		uint8_t chr_idx() const { return m_seq_id.second; }
		
		bool get_alt(std::size_t const lineno, uint8_t &alt_idx) const
		{
			auto it(m_alt_indices.find(lineno));
			if (m_alt_indices.cend() == it)
				return false;
			
			alt_idx = it->second;
			return true;
		}
	
		void set_start_pos(std::size_t const zero_based_pos) { m_start_pos_1 = 1 + zero_based_pos; }
	
		bool equal_sequences(variant_sequence const &other) const
		{
			if (m_start_pos_1 != other.m_start_pos_1)
				return false;
		
			if (m_alt_indices.size() != other.m_alt_indices.size())
				return false;
		
			return std::equal(
				m_alt_indices.cbegin(),
				m_alt_indices.cend(),
				other.m_alt_indices.cbegin(),
				other.m_alt_indices.cend()
			);
		}
	
		void reset()
		{
			m_alt_indices.clear();
		}
	
		void add_alt(std::size_t const lineno, std::size_t const zero_based_pos, uint8_t const alt_idx)
		{
			m_alt_indices[lineno] = alt_idx;
			m_end_pos = 1 + zero_based_pos;
		}
	
		bool assign_id(variant_sequence_id const &seq_id)
		{
			if (m_start_pos_1)
				return false;
		
			m_seq_id = seq_id;
			return true;
		}
	};

	std::ostream &operator<<(std::ostream &stream, variant_sequence const &seq);

	//typedef boost::container::vector <boost::container::map <std::size_t, variant_sequence>> range_map;
	typedef std::vector <std::map <std::size_t, variant_sequence>> range_map;
	
	
	struct sample_reducer_delegate : public virtual variant_processor_delegate
	{
		virtual vcf_reader::sample_name_map const &sample_names() const = 0;
	};
	
	
	class sample_reducer
	{
	protected:
		typedef std::map <std::size_t, boost::container::list <variant_sequence>> subsequence_map;

	protected:
		range_map											*m_compressed_ranges{};
		sample_reducer_delegate								*m_delegate{};
		std::map <variant_sequence_id, variant_sequence>	m_variant_sequences;	// Variant sequences by sample number.
		subsequence_map										m_prepared_sequences;
		std::size_t											m_padding_amt{};
		std::size_t											m_last_position{};
		bool												m_output_ref{};
		bool												m_allow_switch_to_ref{false};
		
	public:
		sample_reducer(
			range_map			&compressed_ranges,
			std::size_t const	padding_amt,
			bool const			output_ref,
			bool const			allow_switch_to_ref
		):
			m_compressed_ranges(&compressed_ranges),
			m_padding_amt(padding_amt),
			m_output_ref(output_ref),
			m_allow_switch_to_ref(allow_switch_to_ref)
		{
		}
		
		void set_delegate(sample_reducer_delegate &delegate) { m_delegate = &delegate; }
		range_map &compressed_ranges() { return *m_compressed_ranges; }
		
		void prepare();
		void handle_variant(variant &var);
		void finish();
		
	protected:
		bool prepared_contains_sequence(variant_sequence const &seq) const;
		void check_and_copy_seq_to_prepared(variant_sequence &seq);
		bool check_variant_sequence(
			variant_sequence &seq,
			variant_sequence_id const &seq_id,
			std::size_t const zero_based_pos
		);
		void assign_ranges_greedy();
		void print_prepared_sequences();
	};
}

#endif
