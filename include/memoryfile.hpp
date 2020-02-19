#ifndef CARE_MEMORY_FILE
#define CARE_MEMORY_FILE

#include <vector>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <functional>
#include <memory>
#include <filesort.hpp>

/*
    T must have a function size_t heapsize() which returns the size 
    of memory allocated on the heap which is owned by an instance of T

    T must have bool writeToBinaryStream(std::ostream& s) const;
    T must have bool readFromBinaryStream(std::istream& s);
*/
template<class T>
struct MemoryFile{

    struct Twrapper{

        Twrapper() = default;
        Twrapper(const Twrapper&) = default;
        Twrapper(Twrapper&&) = default;
        Twrapper& operator=(const Twrapper&) = default;
        Twrapper& operator=(Twrapper&&) = default;

        Twrapper(const T& rhs){
            data = rhs;
        }
        Twrapper(T&& rhs){
            data = std::move(rhs);
        }
        Twrapper& operator=(const T& rhs){
            data = rhs;
            return *this;
        }
        Twrapper& operator=(T&& rhs){
            data = std::move(rhs);
            return *this;
        }

        T data;

        friend std::ostream& operator<<(std::ostream& s, const Twrapper& i){
            i.data.writeToBinaryStream(s);
            return s;
        }

        friend std::istream& operator>>(std::istream& s, Twrapper& i){
            i.data.readFromBinaryStream(s);
            return s;
        }
    };

    struct Reader{
        Reader() = default;
        Reader(const std::vector<T>& vec, std::string filename)
                : 
                memoryiterator(vec.begin()),
                memoryend(vec.end()),
                fileinputstream(std::ifstream(filename, std::ios::binary)),
                fileiterator(std::istream_iterator<Twrapper>(fileinputstream)),
                fileend(std::istream_iterator<Twrapper>()){
        }

        bool hasNext() const{
            return (memoryiterator != memoryend) || (fileiterator != fileend);
        }

        const T* next(){
            assert(hasNext());

            if(memoryiterator != memoryend){
                const T* data = &(*memoryiterator);
                ++memoryiterator;
                return data;
            }else{
                currentFileElement = std::move(*fileiterator);
                ++fileiterator;

                return &(currentFileElement.data);
            }
        }

        Twrapper currentFileElement;
        typename std::vector<T>::const_iterator memoryiterator;
        typename std::vector<T>::const_iterator memoryend;
        std::ifstream fileinputstream;
        std::istream_iterator<Twrapper> fileiterator;
        std::istream_iterator<Twrapper> fileend;
    };

    MemoryFile() = default;

    MemoryFile(std::size_t memoryLimit, std::string file)
            : MemoryFile(memoryLimit, file, [](const T&){return 0;}){
    }

    template<class Func>
    MemoryFile(std::size_t memoryLimit, std::string file, Func&& heapUsageFunc)
        : maxMemoryOfVectorAndHeap(memoryLimit),
          filename(file),
          getHeapUsageOfElement(std::move(heapUsageFunc)){

        outputstream =  std::ofstream(filename, std::ios::binary);
    }

    bool storeElement(T element){
        return store(std::move(element));
    }

    template<class Tcomparator>
    void sort(const std::string& tempdir, Tcomparator&& comparator){

        std::cerr << vector.size() << " elements stored in memory\n";

        //try to sort vector in memory
        bool vectorCouldBeSortedInMemory = false;
        if(onlyInMemory()){
            try{
                std::sort(vector.begin(), vector.end(), comparator);
                vectorCouldBeSortedInMemory = true;
            }catch(std::bad_alloc& e){
                ; //nothing
            }
        }

        if(vectorCouldBeSortedInMemory && onlyInMemory()){
            ; //done
            return;
        }

        //append unsorted vector to file

        for(auto&& element : vector){
            auto wrap = Twrapper{std::move(element)};
            outputstream << wrap;
        }

        outputstream.flush();

        vector = std::vector<T>(); //free memory of vector

        //perform mergesort on file
        auto wrappercomparator = [&](const auto& l, const auto& r){
            return comparator(l.data, r.data);
        };
        auto wrapperheapusage = [&](const auto& w){
            return getHeapUsageOfElement(w.data);
        };

        care::filesort::binKeySort<Twrapper>(tempdir,
                        {filename}, 
                        filename+"2",
                        wrappercomparator,
                        wrapperheapusage);

        renameFileSameMount(filename+"2", filename);

        outputstream = std::ofstream(filename, std::ios::binary | std::ios::app);
    }

    Reader makeReader() const{
        return Reader{vector, filename};
    }

    void flush(){
        outputstream.flush();
    }

    bool onlyInMemory() const{
        return !isUsingFile;
    }

private:

    bool store(T&& element){
        if(!isUsingFile){
            auto getAvailableMemoryInBytes = [](){
                const std::size_t availableMemoryInKB = getAvailableMemoryInKB();
                return availableMemoryInKB << 10;
            };
            
            auto getMemLimit = [&](){
                size_t availableMemory = getAvailableMemoryInBytes();

                constexpr std::size_t oneGB = std::size_t(1) << 30; 
                constexpr std::size_t safetybuffer = oneGB;

                if(availableMemory > safetybuffer){
                    availableMemory -= safetybuffer;
                }else{
                    availableMemory = 0;
                }
                if(availableMemory > oneGB){
                    //round down to next multiple of 1GB
                    availableMemory = (availableMemory / oneGB) * oneGB;
                }
                return availableMemory;
            };

            //size_t memLimit = getMemLimit();

            if(numStoredElements < 2 || numStoredElements % 65536 == 0){
                maxMemoryOfVectorAndHeap = getMemLimit();
            }
            

            //check if element could be saved in memory, disregaring vector growth
            if(vector.capacity() * sizeof(T) + usedHeapMemory + getHeapUsageOfElement(element) <= maxMemoryOfVectorAndHeap){
                if(vector.size() < vector.capacity()){
                    bool retval = true;

                    try{
                        retval = storeInMemory(std::move(element));
                    }catch(std::bad_alloc& e){
                        std::cerr << "switch to file storage after " << numStoredElements << " insertions.\n";
                        isUsingFile = true;
                        retval = storeInFile(std::move(element));
                    }

                    return retval;
                }else{ //size == capacity
                    
                    if(2 * vector.capacity() * sizeof(T) + usedHeapMemory + getHeapUsageOfElement(element) <= maxMemoryOfVectorAndHeap){
                        bool retval = true;

                        try{
                            retval = storeInMemory(std::move(element));
                        }catch(std::bad_alloc& e){
                            std::cerr << "switch to file storage after " << numStoredElements << " insertions.\n";
                            isUsingFile = true;
                            retval = storeInFile(std::move(element));
                        }

                        return retval;
                    }else{
                        std::cerr << "switch to file storage after " << numStoredElements << " insertions.\n";
                        isUsingFile = true;
                        return storeInFile(std::move(element));
                    }
                }
            }else{
                std::cerr << "switch to file storage after " << numStoredElements << " insertions.\n";
                isUsingFile = true;
                return storeInFile(std::move(element));
            }
        }else{
            return storeInFile(std::move(element));
        }
    }

    bool storeInMemory(T&& element){
        usedHeapMemory += getHeapUsageOfElement(element);
        vector.emplace_back(std::move(element));        
        numStoredElements++;

        return true;
    }

    bool storeInFile(T&& element){
        outputstream << Twrapper{std::move(element)};
        numStoredElements++;

        return bool(outputstream);
    }

    bool isUsingFile = false; //if at least 1 element has been written to file
    std::int64_t numStoredElements = 0; // number of stored elements
    std::size_t usedHeapMemory = 0;
    std::size_t maxMemoryOfVectorAndHeap = 0; // usedMemoryOfVector <= maxMemoryOfVector must always hold
    std::function<std::size_t(const T&)> getHeapUsageOfElement;
    std::vector<T> vector; //elements stored in memory
    std::ofstream outputstream;
    std::string filename = "";    
};

















template<class T>
struct MemoryFileFixedSize{

    struct Twrapper{

        Twrapper() = default;
        Twrapper(const Twrapper&) = default;
        Twrapper(Twrapper&&) = default;
        Twrapper& operator=(const Twrapper&) = default;
        Twrapper& operator=(Twrapper&&) = default;

        Twrapper(const T& rhs){
            data = rhs;
        }
        Twrapper(T&& rhs){
            data = std::move(rhs);
        }
        Twrapper& operator=(const T& rhs){
            data = rhs;
            return *this;
        }
        Twrapper& operator=(T&& rhs){
            data = std::move(rhs);
            return *this;
        }

        T data;

        friend std::ostream& operator<<(std::ostream& s, const Twrapper& i){
            i.data.writeToBinaryStream(s);
            return s;
        }

        friend std::istream& operator>>(std::istream& s, Twrapper& i){
            i.data.readFromBinaryStream(s);
            return s;
        }

        std::uint8_t* copyToContiguousMemory(std::uint8_t* begin, std::uint8_t* end) const{
            return data.copyToContiguousMemory(begin, end);
        }

        void copyFromContiguousMemory(const std::uint8_t* begin){
            data.copyFromContiguousMemory(begin);
        }
    };

    struct Reader{
        Reader() = default;
        Reader(
            const std::uint8_t* rawData_, 
            const std::size_t* elementOffsets_, 
            std::size_t numElementsInMemory_, 
            std::string filename)
                : 
                rawData(rawData_),
                elementOffsets(elementOffsets_),
                numElementsInMemory(numElementsInMemory_),
                fileinputstream(std::ifstream(filename, std::ios::binary)),
                fileiterator(std::istream_iterator<Twrapper>(fileinputstream)),
                fileend(std::istream_iterator<Twrapper>()){
        }

        bool hasNext() const{
            return (elementIndexInMemory != numElementsInMemory) || (fileiterator != fileend);
        }

        const T* next(){
            assert(hasNext());

            if(elementIndexInMemory != numElementsInMemory){
                const std::uint8_t* const ptr = rawData + elementOffsets[elementIndexInMemory];
                currentMemoryElement.copyFromContiguousMemory(ptr);

                ++elementIndexInMemory;
                return &currentMemoryElement;
            }else{
                currentFileElement = std::move(*fileiterator);
                ++fileiterator;

                return &(currentFileElement.data);
            }
        }

        const std::uint8_t* rawData;
        const std::size_t* elementOffsets;

        std::size_t elementIndexInMemory = 0;
        std::size_t numElementsInMemory;
        T currentMemoryElement;

        Twrapper currentFileElement;
        std::ifstream fileinputstream;
        std::istream_iterator<Twrapper> fileiterator;
        std::istream_iterator<Twrapper> fileend;
    };

    MemoryFileFixedSize() = default;

    MemoryFileFixedSize(std::size_t memoryLimitBytes, std::int64_t maxElementsInMemory_, std::string file)
        : 
          maxElementsInMemory(maxElementsInMemory_),
          filename(file),
          outputstream(filename, std::ios::binary){

        assert(outputstream.good());

        const std::size_t memoryForOffsets = maxElementsInMemory * sizeof(std::size_t);
        if(memoryForOffsets < memoryLimitBytes){
            elementOffsets = std::make_unique<std::size_t[]>(maxElementsInMemory);
            memoryLimitBytes -= memoryForOffsets;

            if(memoryLimitBytes > 0){
                rawData = std::make_unique<std::uint8_t[]>(memoryLimitBytes);
                rawDataBytes = memoryLimitBytes;
                currentDataPtr = rawData.get();
            }else{
                elementOffsets = nullptr;
                rawData = nullptr;
                rawDataBytes = 0;
                currentDataPtr = rawData.get();
            }
        }else{
            elementOffsets = nullptr;
            rawData = nullptr;
            rawDataBytes = 0;
            currentDataPtr = rawData.get();
        }
    }

    Reader makeReader() const{
        return Reader{rawData.get(), elementOffsets.get(), getNumElementsInMemory(), filename};
    }

    void flush(){
        outputstream.flush();
    }

    bool storeElement(T&& element){
        T tmp(std::move(element));

        if(!isUsingFile){
            bool success = storeInMemory(tmp);
            if(!success){
                isUsingFile = true;
                success = storeInFile(std::move(tmp));
            }
            return success;
        }else{
            return storeInFile(std::move(tmp));
        }
    }

    std::int64_t getNumElementsInMemory() const{
        return numStoredElementsInMemory;
    }

    std::int64_t getNumElementsInFile() const{
        return numStoredElementsInFile;
    }

    std::int64_t getNumElements() const{
        return getNumElementsInMemory() + getNumElementsInFile();
    }

    template<class Ptrcomparator, class TComparator>
    void sort(const std::string& tempdir, Ptrcomparator&& ptrcomparator, TComparator&& elementcomparator){
    //void sort(const std::string& tempdir){
        std::cerr << "Sorting memory file:";
        std::cerr << " elements in memory = " << getNumElementsInMemory();
        std::cerr << " elements in file = " << getNumElementsInFile();
        std::cerr << '\n';

        auto offsetcomparator = [&](std::size_t elementOffset1, std::size_t elementOffset2){
            return ptrcomparator(rawData.get() + elementOffset1, rawData.get() + elementOffset2);
        };

        //try to sort vector in memory
        bool vectorCouldBeSortedInMemory = false;
        if(getNumElementsInFile() == 0){
            try{
                std::sort(elementOffsets.get(), elementOffsets.get() + getNumElementsInMemory(), offsetcomparator);
                vectorCouldBeSortedInMemory = true;
            }catch(std::bad_alloc& e){
                ; //nothing
            }
        }

        if(vectorCouldBeSortedInMemory && getNumElementsInFile() == 0){
            ; //done
            return;
        }

        //append unsorted vector to file
        std::size_t memoryBytes = std::distance(rawData.get(), currentDataPtr);
        outputstream.write(reinterpret_cast<const char*>(rawData.get()), memoryBytes);

        outputstream.flush();

        rawData = nullptr;
        elementOffsets = nullptr;
        std::size_t numElements = getNumElements();
        numStoredElementsInFile = numElements;
        numStoredElementsInMemory = 0;

        //perform mergesort on file
        auto wrapperptrcomparator = [&](const std::uint8_t* l, const std::uint8_t* r){
            return ptrcomparator(l, r);
        };

        auto wrappercomparator = [&](const auto& l, const auto& r){
            return elementcomparator(l.data, r.data);
        };

        const std::size_t availableMemoryInBytes = getAvailableMemoryInKB() * 1024;
        std::size_t memoryForSorting = 0;

        if(availableMemoryInBytes > (std::size_t(1) << 30)){
            memoryForSorting = availableMemoryInBytes - (std::size_t(1) << 30);
        }

        assert(memoryForSorting > 0);

        care::filesort::fixedmemory::binKeySort<Twrapper>(tempdir,
                        {filename}, 
                        filename+"2",
                        memoryForSorting,
                        wrapperptrcomparator,
                        wrappercomparator);

        renameFileSameMount(filename+"2", filename);

        outputstream = std::ofstream(filename, std::ios::binary | std::ios::app);
    }

private:
    bool storeInMemory(const T& element){
        if(getNumElementsInMemory() >= maxElementsInMemory){
            return false;
        }

        std::uint8_t* const newDataPtr = element.copyToContiguousMemory(currentDataPtr, rawData.get() + rawDataBytes);
        if(newDataPtr != nullptr){
            elementOffsets[numStoredElementsInMemory] = std::distance(rawData.get(), currentDataPtr);

            currentDataPtr = newDataPtr;
            numStoredElementsInMemory++;
            return true;
        }else{
            return false;
        }
    }

    bool storeInFile(T&& element){
        outputstream << Twrapper{std::move(element)};
        bool result = bool(outputstream);
        if(result){
            numStoredElementsInFile++;
        }
        
        return result;
    }

    bool isUsingFile = false;

    std::unique_ptr<std::uint8_t[]> rawData = nullptr;
    std::size_t rawDataBytes = 0;
    std::uint8_t* currentDataPtr = nullptr;

    std::unique_ptr<std::size_t[]> elementOffsets = nullptr;
    std::int64_t maxElementsInMemory = 0;    
    
    std::int64_t numStoredElementsInMemory = 0;
    std::int64_t numStoredElementsInFile = 0;

    std::string filename = "";    
    std::ofstream outputstream;
};

#endif