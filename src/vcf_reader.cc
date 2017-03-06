/*
 Copyright (c) 2017 Tuukka Norri
 This code is licensed under MIT license (see LICENSE for details).
 */

#include <iostream>
#include <vcf2multialign/vcf_reader.hh>


namespace vcf2multialign {
	
	size_t variant::pos()
	{
		if (0 == m_pos)
			detail::parse_int(m_var_fields[detail::to_underlying(vcf_field::POS)], m_pos);
		
		return m_pos;
	}
	
	
	std::vector <std::string_view> const &variant::alt()
	{
		if (!m_parsed_alt)
			detail::read_fields <true>(m_var_fields[detail::to_underlying(vcf_field::ALT)], ",", -1, m_alt);
		
		return m_alt;
	}
	
	
	void variant::reset()
	{
		m_alt.clear();
		m_pos = 0;
		m_parsed_alt = false;
		m_format = "";
		m_format_fields.clear();
		m_format_max = 0;
		m_current_sample_no = 0;
	}
	
	
	void variant::map_format_fields(std::string_view const &format)
	{
		m_format_fields.clear();
		auto format_fields(m_requested_format_fields);
		char const *string_start(static_cast <char const *>(format.data()));
		char const *string_ptr(string_start);
		uint8_t i(0);
		
		std::string_view::size_type sep_pos(0);
		std::string buffer;
		bool should_break(false);
		while (true)
		{
			sep_pos = format.find(":", sep_pos);
			if (std::string_view::npos == sep_pos)
			{
				should_break = true;
				sep_pos = format.size();
			}
			std::string_view sv(string_ptr, sep_pos - (string_ptr - string_start));
			
			buffer.assign(sv.cbegin(), sv.cend());
			auto it(format_fields.find(buffer));
			if (format_fields.cend() != it)
			{
				m_format_fields.insert(std::make_pair(buffer, i));
				m_format_max = i;
			}
			
			++i;

			if (0 == format_fields.size())
				break;
			
			if (should_break)
				break;
			
			string_ptr = string_start + sep_pos;
			++sep_pos;
		}
		
		m_sample_fields.resize(i);
		m_format.assign(format.cbegin(), format.cend());
	}
	
	
	void variant::parse_sample(size_t const sample_no)
	{
		auto const idx(detail::to_underlying(vcf_field::ALL) + sample_no - 1);
		auto const &val(m_var_fields[idx]);
		detail::read_fields <false>(val, ":", 1 + m_format_max, m_sample_fields);
		m_current_sample_no = sample_no;
	}

	
	// FIXME: change this like so in order to enable threading:
	// prepare_samples() checks format and calls map_format_fields if needed.
	// parse_sample() takes a vector to which it places the sample fields. (Use vector_source?)
	// get_genotype() takes the vector as well as the out-parameters listed below and fills them.
	void variant::get_genotype(size_t const sample_no, std::vector <uint8_t> &res, bool &phased)
	{
		auto const &format(m_var_fields[detail::to_underlying(vcf_field::FORMAT)]);
		if (m_format != format)
			map_format_fields(format);
		
		if (sample_no != m_current_sample_no)
			parse_sample(sample_no);
		
		res.clear();
		uint8_t alt_num(0);
		phased = true;
		for (auto c : m_sample_fields[m_format_fields["GT"]])
		{
			if ('0' <= c && c <= '9')
			{
				alt_num *= 10;
				alt_num += c - '0';
			}
			else
			{
				if ('/' == c)
					phased = false;
				
				res.emplace_back(alt_num);
				alt_num = 0;
			}
		}
		
		res.emplace_back(alt_num);
	}
	
	
	void vcf_reader::read_header()
	{
		std::vector <std::string_view> fields(m_parsed_field_count);
		
		// For now, just skip lines that begin with "##".
		while (std::getline(*m_stream, m_line))
		{
			++m_lineno;
			if (! ('#' == m_line[0] && '#' == m_line[1]))
				break;
			
			// FIXME: header handling goes here.
		}
		
		// Check for column headers.
		{
			auto const prefix(std::string("#CHROM"));
			auto const res(mismatch(m_line.cbegin(), m_line.cend(), prefix.cbegin(), prefix.cend()));
			if (prefix.cend() != res.second)
			{
				std::cerr << "Expected the next line to start with '#CHROM', instead got: '" << m_line << "'" << std::endl;
				exit(1);
			}
		}
		
		// Read sample names.
		std::vector <std::string_view> header_names;
		detail::read_fields <true>(m_line, "\t", -1, header_names);
		std::size_t i(1);
		for (auto it(header_names.cbegin() + detail::to_underlying(vcf_field::ALL)), end(header_names.cend()); it != end; ++it)
		{
			std::string str(*it);
			auto const res(m_sample_names.emplace(std::move(str), i));
			if (!res.second)
				throw std::runtime_error("Duplicate sample name");
			++i;
		}
		
		// stream now points to the first variant.
		m_first_variant_offset = m_stream->tellg();
		m_last_header_lineno = m_lineno;
	}
	
	
	void vcf_reader::reset()
	{
		m_stream->seekg(m_first_variant_offset);
		m_lineno = m_last_header_lineno;
	}
	
	
	bool vcf_reader::get_next_variant(variant &var)
	{
		if (!std::getline(*m_stream, m_line))
			return false;
		
		++m_lineno;
		var.reset();
		detail::read_fields <false>(m_line, "\t", m_parsed_field_count, var.m_var_fields);
		return true;
	}
	
	
	size_t vcf_reader::sample_no(std::string const &sample_name) const
	{
		auto const it(m_sample_names.find(sample_name));
		if (it == m_sample_names.cend())
			return 0;
		return it->second;
	}
	
	
	size_t vcf_reader::sample_count() const
	{
		return m_sample_names.size();
	}
	
	
	void vcf_reader::set_parsed_fields(vcf_field const last_field)
	{
		if (last_field == vcf_field::ALL)
			m_parsed_field_count = sample_count() + detail::to_underlying(vcf_field::ALL);
		else
			m_parsed_field_count = 1 + detail::to_underlying(last_field);
	}
}
