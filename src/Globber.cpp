/*
 * Copyright 2015-2016 Gary R. Van Sickle (grvs@users.sourceforge.net).
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

#include "Globber.h"

#include "TypeManager.h"
#include "DirInclusionManager.h"

#include <fts.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <utility>


Globber::Globber(std::vector<std::string> start_paths,
		TypeManager &type_manager,
		DirInclusionManager &dir_inc_manager,
		bool recurse_subdirs,
		sync_queue<std::string>& out_queue)
		: m_start_paths(start_paths),
		  m_out_queue(out_queue),
		  m_type_manager(type_manager),
		  m_dir_inc_manager(dir_inc_manager),
		  m_recurse_subdirs(recurse_subdirs)
{

}

Globber::~Globber()
{

}

void Globber::Run()
{
	char * dirs[m_start_paths.size()+1];

	/// @todo It looks like OSX needs any trailing slashes to be removed here, or its fts lib will double them up.
	/// Doesn't seem to affect results though.

	int i = 0;
	for(const std::string& path : m_start_paths)
	{
		dirs[i] = const_cast<char*>(path.c_str());

		// Check if this start path exists and is a file or directory.
		DIR *d = opendir(dirs[i]);
		int f = open(dirs[i], O_RDONLY);

		if((d==NULL) && (f==-1))
		{
			m_bad_path = dirs[i];
		}

		// Close the dir/file we opened.
		if(d != NULL)
		{
			closedir(d);
		}
		if(f != -1)
		{
			close(f);
		}

		if(!m_bad_path.empty())
		{
			// If we couldn't open the specified file/dir, we don't start the globbing, but ultimately
			// return to main() and exit with an error.
			return;
		}

		++i;
	}
	dirs[m_start_paths.size()] = 0;

	/// @todo We can't use FS_NOSTAT here.  OSX at least isn't able to determine regular
	/// files without the stat, so they get returned as FTS_NSOK / 11 /	no stat(2) requested.
	/// Does not seem to affect performance on Linux, but might be having an effect on Cygwin.
	/// Look into workarounds.
	FTS *fts = fts_open(dirs, FTS_LOGICAL | FTS_NOCHDIR /*| FTS_NOSTAT*/, NULL);
	while(FTSENT *ftsent = fts_read(fts))
	{
		//std::clog << "Considering file: " << ftsent->fts_path << std::endl;
		if(ftsent->fts_info == FTS_F)
		{
			//std::clog << "... normal file." << std::endl;
			// It's a normal file.  Check for inclusion.
			if(m_type_manager.FileShouldBeScanned(ftsent->fts_name))
			{
				//std::clog << "... should be scanned." << std::endl;
				// Extension was in the hash table.
				m_out_queue.wait_push(std::string(ftsent->fts_path));

				// Count the number of files we found that were included in the search.
				m_num_files_found++;
			}
		}
		else if(ftsent->fts_info == FTS_D)
		{
			//std::clog << "... directory." << std::endl;
			// It's a directory.  Check if we should descend into it.
			if(!m_recurse_subdirs && ftsent->fts_level > 0)
			{
				// We were told not to recurse into subdirectories.
				fts_set(fts, ftsent, FTS_SKIP);
			}
			if(m_dir_inc_manager.DirShouldBeExcluded(ftsent->fts_path, ftsent->fts_name))
			{
				// This name is in the dir exclude list.  Exclude the dir and all subdirs from the scan.
				fts_set(fts, ftsent, FTS_SKIP);
			}
		}
		else if(ftsent->fts_info == FTS_DNR)
		{
			// A directory that couldn't be read.
			std::cerr << "ucg: ERROR: unable to read directory \"" << ftsent->fts_path << "\", skipping." << std::endl;
		}
		else if(ftsent->fts_info == FTS_ERR)
		{
			//std::clog << "... FTS_ERR." << std::endl;
			m_bad_path = ftsent->fts_path;
			break;
		}
		else
		{
			//std::clog << "... unknown file type:" << ftsent->fts_info << std::endl;
		}
	}
	fts_close(fts);

	//std::clog << "NUM FILES INCLUDED: " << m_num_files_found << std::endl;
}

