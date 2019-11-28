#ifndef PEER_ACCESS_HELPER_HPP
#define PEER_ACCESS_HELPER_HPP

#ifdef __NVCC__

#include <cassert>
#include <iostream>
#include <vector>


    #ifndef CUERR

        #define CUERR {                                                            \
            cudaError_t err;                                                       \
            if ((err = cudaGetLastError()) != cudaSuccess) {                       \
                std::cerr << "CUDA error: " << cudaGetErrorString(err) << " : "    \
                          << __FILE__ << ", line " << __LINE__ << std::endl;       \
                exit(1);                                                           \
            }                                                                      \
        }

    #endif

    enum class PeerAccessDebugMode {Enabled, Disabled};

    template<PeerAccessDebugMode dbg>
    struct PeerAccessBase{
        static constexpr PeerAccessDebugMode debugmode = dbg;

        bool resetOnDestruction;
        int numGpus;
        std::vector<int> accessMatrix;
        std::vector<int> oldEnabledPeerAccesses;

        PeerAccessBase() : PeerAccessBase(true){}

        PeerAccessBase(bool resetOnDestruction_)
            : resetOnDestruction(resetOnDestruction_){

            cudaGetDeviceCount(&numGpus); CUERR;
            accessMatrix.resize(numGpus * numGpus);
            for(int i = 0; i < numGpus; i++){
                for(int k = 0; k < numGpus; k++){
                    //device i can access device k?
                    cudaDeviceCanAccessPeer(&accessMatrix[i * numGpus + k], i, k); CUERR;
                }
            }

            if(resetOnDestruction){
                //save current enabled peer accesses
                oldEnabledPeerAccesses = getEnabledPeerAccesses();
            }
        }

        ~PeerAccessBase(){
            if(resetOnDestruction && int(oldEnabledPeerAccesses.size()) == numGpus * numGpus){
                setEnabledPeerAccesses(oldEnabledPeerAccesses);
            }
        }

        PeerAccessBase(const PeerAccessBase&) = default;
        PeerAccessBase(PeerAccessBase&&) = default;
        PeerAccessBase& operator=(const PeerAccessBase&) = default;
        PeerAccessBase& operator=(PeerAccessBase&&) = default;

        bool canAccessPeer(int device, int peerDevice) const{
            assert(device < numGpus);
            assert(peerDevice < numGpus);
            return accessMatrix[device * numGpus + peerDevice] == 1;
        }

        void enablePeerAccess(int device, int peerDevice) const{
            assert(canAccessPeer(device, peerDevice));
            int oldId; cudaGetDevice(&oldId); CUERR;
            cudaSetDevice(device); CUERR;
            cudaError_t status = cudaDeviceEnablePeerAccess(peerDevice, 0);
            if(status != cudaSuccess){
                if(status == cudaErrorPeerAccessAlreadyEnabled){
                    if(debugmode == PeerAccessDebugMode::Enabled){
                        std::cerr << "Peer access from " << device << " to " << peerDevice << " has already been enabled. This is not a program error\n";
                    }
                    cudaGetLastError(); //reset error state;
                }else{
                    CUERR;
                }
            }
            cudaSetDevice(oldId); CUERR;
        }

        void disablePeerAccess(int device, int peerDevice) const{
            assert(canAccessPeer(device, peerDevice));
            int oldId; cudaGetDevice(&oldId); CUERR;
            cudaSetDevice(device); CUERR;
            cudaError_t status = cudaDeviceDisablePeerAccess(peerDevice);
            if(status != cudaSuccess){
                if(status == cudaErrorPeerAccessNotEnabled){
                    if(debugmode == PeerAccessDebugMode::Enabled){
                        std::cerr << "Peer access from " << device << " to " << peerDevice << " has not yet been enabled. This is not a program error\n";
                    }
                    cudaGetLastError(); //reset error state;
                }else{
                    CUERR;
                }
            }
            cudaSetDevice(oldId); CUERR;
        }

        void enableAllPeerAccesses(){
            for(int i = 0; i < numGpus; i++){
                for(int k = 0; k < numGpus; k++){
                    if(canAccessPeer(i, k)){
                        enablePeerAccess(i, k);
                    }
                }
            }
        }

        void disableAllPeerAccesses(){
            for(int i = 0; i < numGpus; i++){
                for(int k = 0; k < numGpus; k++){
                    if(canAccessPeer(i, k)){
                        disablePeerAccess(i, k);
                    }
                }
            }
        }

        std::vector<int> getEnabledPeerAccesses() const{
            int numGpus = 0;
            cudaGetDeviceCount(&numGpus); CUERR;

            std::vector<int> result(numGpus * numGpus, 0);

            if(numGpus > 0){
                int oldId; cudaGetDevice(&oldId); CUERR;

                for(int i = 0; i < numGpus; i++){
                    cudaSetDevice(i); CUERR;
                    for(int k = 0; k < numGpus; k++){
                        if(canAccessPeer(i,k)){
                            cudaError_t status = cudaDeviceDisablePeerAccess(k);
                            if(status == cudaSuccess){
                                //if device i can disable access to device k, it must have been enabled
                                result[i * numGpus + k] = 1;
                                //enable again
                                cudaDeviceEnablePeerAccess(k, 0); CUERR;
                            }else{
                                if(status != cudaErrorPeerAccessNotEnabled){
                                    CUERR; //error
                                }
                                cudaGetLastError(); //reset error state;
                            }
                        }
                    }
                }

                cudaSetDevice(oldId);
            }

            return result;
        }

        std::vector<int> getDisabledPeerAccesses() const{
            std::vector<int> result = getEnabledPeerAccesses();
            for(auto& i : result){
                i = 1-i; // 0->1, 1->0
            }
            return result;
        }

        void setEnabledPeerAccesses(const std::vector<int>& vec){
            for(int i = 0; i < numGpus; i++){
                for(int k = 0; k < numGpus; k++){
                    if(canAccessPeer(i,k)){
                        int flag = vec[i * numGpus + k];
                        if(flag == 1){
                            enablePeerAccess(i,k);
                        }else{
                            disablePeerAccess(i,k);
                        }
                    }
                }
            }
        }
    };

    using PeerAccess = PeerAccessBase<PeerAccessDebugMode::Disabled>;
    using PeerAccessDebug = PeerAccessBase<PeerAccessDebugMode::Enabled>;

#endif

#endif
