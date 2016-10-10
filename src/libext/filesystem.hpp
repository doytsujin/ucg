/*
 * Copyright 2016 Gary R. Van Sickle (grvs@users.sourceforge.net).
 *
 * This file is part of UniversalCodeGrep.
 *
 * UniversalCodeGrep is free software: you can redistribute it and/or modify it under the
 * terms of version 3 of the GNU General Public License as published by the Free
 * Software Foundation.
 *
 * UniversalCodeGrep is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * UniversalCodeGrep.  If not, see <http://www.gnu.org/licenses/>.
 */

/** @file */

#ifndef SRC_LIBEXT_FILESYSTEM_HPP_
#define SRC_LIBEXT_FILESYSTEM_HPP_

#include <config.h>

#include <fcntl.h> // For openat() etc.
#include <unistd.h> // For close().
#include <sys/stat.h>
#include <sys/types.h> // for dev_t, ino_t
// Don't know where the name "libgen" comes from, but this is where POSIX says dirname() and basename() are declared.
/// There are two basename()s.  GNU basename, from string.h, and POSIX basename() from libgen.h.
/// See notes here: https://linux.die.net/man/3/dirname
/// Of course they behave slightly differently: GNU version returns an empty string if the path has a trailing slash, and doesn't modify it's argument.
/// To complicate matters further, the glibc version of the POSIX function versions do modify their args.
/// And at least NetBSD behaves similarly.  So, include the POSIX versions and we'll try to clean this mess up below.
#include <libgen.h>
#include <dirent.h>

/// @note Because we included libgen.h above, we shouldn't get the GNU version from this #include of string.h.
#include <string.h>
#include <cstdlib>   // For free().
#include <string>

#include "integer.hpp"
#include "../Logger.h"

/// @name Take care of some portability issues.
/// OSX (clang-600.0.54) (based on LLVM 3.5svn)/x86_64-apple-darwin13.4.0:
/// - No AT_FDCWD, no openat, no fdopendir, no fstatat.
/// Cygwin:
/// - No O_NOATIME, no AT_NO_AUTOMOUNT.
/// Linux:
/// - No O_SEARCH.
/// @{
#ifndef O_SEARCH
// O_SEARCH is POSIX.1-2008, but not defined on at least Linux/glibc 2.24.
// Possible reason, quoted from the standard: "Since O_RDONLY has historically had the value zero, implementations are not able to distinguish
// between O_SEARCH and O_SEARCH | O_RDONLY, and similarly for O_EXEC."
#define O_SEARCH 0
#endif
/// @}


using dev_ino_pair_type = uint_t<(sizeof(dev_t)+sizeof(ino_t))*8>::fast;

struct dev_ino_pair
{
	dev_ino_pair() = default;
	dev_ino_pair(dev_t d, ino_t i) noexcept { m_val = d, m_val <<= sizeof(ino_t)*8, m_val |= i; };

	dev_ino_pair_type m_val { 0 };
};

/**
 * Get the d_name field out of the passed dirent struct #de and into a std::string, in as efficient manner as posible.
 *
 * @param de
 * @return
 */
inline std::string dirent_get_name(const dirent* de) noexcept
{
#if defined(_DIRENT_HAVE_D_NAMLEN)
		// struct dirent has a d_namelen field.
		std::string basename(de->d_name, de->d_namelen);
#elif defined(_DIRENT_HAVE_D_RECLEN) && defined(_D_ALLOC_NAMLEN)
		// We can cheaply determine how much memory we need to allocate for the name.
		/// @todo May not have a strnlen(). // std::string basename(_D_ALLOC_NAMLEN(de), '\0');
		std::string basename(de->d_name, strnlen(de->d_name, _D_ALLOC_NAMLEN(de)));
#else
		// All we have is a null-terminated d_name.
		std::string basename(de->d_name);
#endif

	// RVO should optimize this.
	return basename;
}


#if 1 /// @todo
constexpr int cm_invalid_file_descriptor = -987;
struct FileDescriptorDeleter
{
	void operator()(int *fd) const noexcept
	{
		if(*fd >= 0)
		{
			LOG(INFO) << "Closing file descriptor " << *fd;
			close(*fd);
		}
		delete fd;
	}
};
using FileDescriptor = std::shared_ptr<int>;
inline std::shared_ptr<int> make_shared_fd(int fd) ATTR_ARTIFICIAL;
inline std::shared_ptr<int> make_shared_fd(int fd)
{
	return FileDescriptor(new int(fd), FileDescriptorDeleter());
}

#else
/**
 * Wrapper for C's 'int' file descriptor.
 * This class only adds C++ RAII abilities and correct move semantics to a file descriptor.
 */
class FileDescriptor
{
public:
	FileDescriptor() noexcept = default;

	explicit FileDescriptor(int fd) noexcept { m_file_descriptor = fd; };

	/// Copy constructor will dup the file descriptor.
	FileDescriptor(const FileDescriptor &other) noexcept : FileDescriptor()
	{
		*this = other;
	}

	/// Move constructor.
	/// For a move, the #moved_from FileDescriptor has to be invalidated.  Otherwise,
	/// when it is destroyed, it will close the file, which it no longer owns.
	FileDescriptor(FileDescriptor&& moved_from) noexcept : FileDescriptor()
	{
		// Implement in terms of move-assignment.
		*this = std::move(moved_from);
	}

	/// Destructor.  Closes #m_file_descriptor if it's valid.
	~FileDescriptor()
	{
		if((m_file_descriptor >= 0) && (m_file_descriptor != cm_invalid_file_descriptor))
		{
			close(m_file_descriptor);
		}
	};

	/// The default copy-assignment operator won't do the right thing.
	const FileDescriptor& operator=(const FileDescriptor& other) noexcept
	{
		if(this != &other)
		{
			if((m_file_descriptor >= 0) && (m_file_descriptor != cm_invalid_file_descriptor))
			{
				close(m_file_descriptor);
			}

			if(other.m_file_descriptor < 0)
			{
				m_file_descriptor = other.m_file_descriptor;
			}
			else
			{
				m_file_descriptor = dup(other.m_file_descriptor);
				if((m_file_descriptor < 0) && (m_file_descriptor != AT_FDCWD))
				{
					perror("dup");
				}
			}
		}
		return *this;
	}

	/// The default move-assignment operator won't do the right thing.
	FileDescriptor& operator=(FileDescriptor&& other) noexcept
	{
		if(this != &other)
		{
			// Step 1: Release any resources this owns.
			if((m_file_descriptor >= 0) && (m_file_descriptor != cm_invalid_file_descriptor))
			{
				close(m_file_descriptor);
			}

			// Step 2: Take other's resources.
			m_file_descriptor = other.m_file_descriptor;

			// Step 3: Set other to a destructible state.
			// In particular here, this means invalidating its file descriptor,
			// so it isn't closed when other is deleted.
			other.m_file_descriptor = cm_invalid_file_descriptor;
		}

		// Step 4: Return *this.
		return *this;
	}

	/// Allow read access to the underlying int.
	int GetInt() const noexcept { return m_file_descriptor; };

	/// Returns true if this FileDescriptor has never been assigned a valid file descriptor.
	bool IsInvalid() const noexcept { return m_file_descriptor == cm_invalid_file_descriptor; };

private:
	static constexpr int cm_invalid_file_descriptor = -987;

	int m_file_descriptor { cm_invalid_file_descriptor };
};
#endif


/**
 * Checks two file descriptors (file, dir, whatever) and checks if they are referring to the same entity.
 *
 * @param fd1
 * @param fd2
 * @return true if fd1 and fd2 are fstat()able and refer to the same entity, false otherwise.
 */
inline bool is_same_file(int fd1, int fd2)
{
	struct stat s1, s2;

	if(fstat(fd1, &s1) < 0)
	{
		return false;
	}
	if(fstat(fd2, &s2) < 0)
	{
		return false;
	}

	if(
		(s1.st_dev == s2.st_dev) // Same device
		&& (s1.st_ino == s2.st_ino) // Same inode
		)
	{
		return true;
	}
	else
	{
		return false;
	}
}

namespace portable
{

/**
 * A more usable and portable replacement for glibc and POSIX dirname().
 *
 * @param path  const ref to a path string.  Guaranteed to not be modified in any way by the function call.
 * @return  A std::string representing the path to return the directory of.  Guaranteed to be a normal std::string with which you may do
 *          whatever you can do with any other std::string.
 */
inline std::string dirname(const std::string &path)
{
	// Get a copy of the path string which dirname() can modify all it wants.
	char * modifiable_path = strdup(path.c_str());

	// Copy the output of dirname into a std:string.  We don't ever free the string dirname() returns
	// because it's either a static buffer, or it's a pointer to modifiable_path.  The latter we'll free below.
	std::string retval(::dirname(modifiable_path));

	free(modifiable_path);

	return retval;
}

}

/**
 * Examines the given #path and determines if it is absolute.
 *
 * @param path
 * @return
 */
inline bool is_pathname_absolute(const std::string &path) noexcept
{
#if 1 // == IS_POSIX
	if(path[0] == '/')
	{
		return true;
	}
	else
	{
		return false;
	}
#else /// @todo Handle Windows etc.
#error "Only POSIX-like systems currently supported."
	return false;
#endif
}


inline DIR* opendirat(int at_dir, const char *name)
{
	LOG(INFO) << "Attempting to open directory \'" << name << "\' at file descriptor " << at_dir;
	constexpr int openat_dir_search_flags = O_SEARCH ? O_SEARCH : O_RDONLY;

	int file_fd = openat(at_dir, name, openat_dir_search_flags | O_DIRECTORY | O_NOCTTY);
	if(file_fd < 0)
	{
		perror("openat() failed");
	}
	DIR* d = fdopendir(file_fd);

	return d;
}

#endif /* SRC_LIBEXT_FILESYSTEM_HPP_ */
