#pragma once

#include <string>

// Compare suffixes only; a Unicode slash-like character is part of a name,
// while the actual ASCII slash and backslash delimit path components.
template<typename Char>
bool SamePathExtension(const std::basic_string<Char>& left, const std::basic_string<Char>& right)
{
	const auto extension = [](const std::basic_string<Char>& path) {
		auto dot = path.size();
		for( std::size_t i = 0; i < path.size(); ++i )
		{
			if( sizeof(Char) == 1 )
			{
				const unsigned c = static_cast<unsigned char>(path[i]);
				if( (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc) )
				{ if( i + 1 < path.size() ) ++i; continue; }
			}
			if( path[i] == Char('/') || path[i] == Char('\\') ) dot = path.size();
			else if( path[i] == Char('.') ) dot = i;
		}
		return dot;
	};
	auto i = extension(left), j = extension(right);
	if( left.size() - i != right.size() - j ) return false;
	for( ; i < left.size(); ++i, ++j )
	{
		if( sizeof(Char) == 1 )
		{
			const unsigned aByte = static_cast<unsigned char>(left[i]);
			const unsigned bByte = static_cast<unsigned char>(right[j]);
			const bool aLead = (aByte >= 0x81 && aByte <= 0x9f) || (aByte >= 0xe0 && aByte <= 0xfc);
			const bool bLead = (bByte >= 0x81 && bByte <= 0x9f) || (bByte >= 0xe0 && bByte <= 0xfc);
			if( aLead || bLead )
			{
				if( !aLead || !bLead || left[i] != right[j]
					|| i + 1 >= left.size() || j + 1 >= right.size()
					|| left[i + 1] != right[j + 1] ) return false;
				++i; ++j;
				continue;
			}
		}
		Char a = left[i], b = right[j];
		if( a >= Char('A') && a <= Char('Z') ) a += Char('a' - 'A');
		if( b >= Char('A') && b <= Char('Z') ) b += Char('a' - 'A');
		if( a != b ) return false;
	}
	return true;
}
