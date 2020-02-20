#ifndef FILE_HELPERS_HPP
#define FILE_HELPERS_HPP

#include <cstdio>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <experimental/filesystem>

#define FILE_HELPERS_DEBUG

namespace filesys = std::experimental::filesystem;

namespace filehelpers{

__inline__
void renameFileSameMount(const std::string& filename, const std::string& newFilename){
#ifdef FILE_HELPERS_DEBUG        
    std::cerr << "Rename " << filename << " to " << newFilename << "\n";
#endif    
    int res = std::rename(filename.c_str(), newFilename.c_str());
    if(res != 0){
        std::perror("rename");
        assert(res == 0);
    }    
}

__inline__
void copyFile(const std::string& filename, const std::string& newFilename){
#ifdef FILE_HELPERS_DEBUG       
    std::cerr << "Copy " << filename << " to " << newFilename << "\n";
#endif    
    std::ifstream src(filename, std::ios::binary);
    std::ofstream dst(newFilename, std::ios::binary);
    assert(bool(src));
    assert(bool(dst));
    dst << src.rdbuf();
    assert(bool(dst));
}

__inline__
void removeFile(const std::string& filename){
#ifdef FILE_HELPERS_DEBUG   
    std::cerr << "Remove " << filename << "\n";
#endif    
    std::ifstream src(filename);
    assert(bool(src));
    int ret = std::remove(filename.c_str());
    if (ret != 0){
        const std::string errormessage = "Could not remove file " + filename;
        std::perror(errormessage.c_str());
    }  
}

__inline__ 
bool fileCanBeOpened(const std::string& filename){
    std::ifstream in(filename);
    return bool(in);
}

__inline__ 
void deleteFiles(const std::vector<std::string>& filenames){
    for (const auto& filename : filenames) {
        removeFile(filename);
    }
}

__inline__
std::uint64_t linecount(const std::string& filename){
	std::uint64_t count = 0;
	std::ifstream is(filename);
	if(is){
		std::string s;
		while(std::getline(is, s)){
			++count;
		}
	}
	return count;
}

__inline__
std::string getFileName(std::string filePath){
    filesys::path path(filePath);
    return path.filename().string();
}

} //namespace filehelpers


#ifdef FILE_HELPERS_DEBUG
#undef FILE_HELPERS_DEBUG
#endif

#endif