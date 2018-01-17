#include <paper/working.hpp>

#include <Foundation/Foundation.h>

namespace paper
{
boost::filesystem::path app_path ()
{
	NSString * dir_string = NSHomeDirectory ();
	char const * dir_chars = [dir_string UTF8String];
	boost::filesystem::path result (dir_chars);
	[dir_string release];
	return result;
}
}