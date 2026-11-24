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
			const unsigned c = static_cast<unsigned char>(path[i]);
			if( sizeof(Char) == 1 && ((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) )
			{ if( i + 1 < path.size() ) ++i; continue; }
			if( path[i] == Char('/') || path[i] == Char('\\') ) dot = path.size();
			else if( path[i] == Char('.') ) dot = i;
		}
		return dot;
	};
	auto i = extension(left), j = extension(right);
	if( left.size() - i != right.size() - j ) return false;
	for( ; i < left.size(); ++i, ++j )
	{
		Char a = left[i], b = right[j];
		if( a >= Char('A') && a <= Char('Z') ) a += Char('a' - 'A');
		if( b >= Char('A') && b <= Char('Z') ) b += Char('a' - 'A');
		if( a != b ) return false;
	}
	return true;
}
