#!/bin/bash
# Combined build script for SMB DOS with Allegro 4

DJGPP_IMAGE="djfdyuruiry/djgpp"
BUILD_DIR="build/dos"
USER_ID=$(id -u)
GROUP_ID=$(id -g)

case "${1:-dos}" in

"setup")
    echo "Setting up build environment..."
    docker pull $DJGPP_IMAGE
    
    mkdir -p $BUILD_DIR
    
    # Download CSDPMI
    if [ ! -d "$BUILD_DIR/csdpmi" ]; then
        echo "Downloading CSDPMI..."
        cd $BUILD_DIR
        curl -L -o csdpmi7b.zip http://na.mirror.garr.it/mirrors/djgpp/current/v2misc/csdpmi7b.zip
        unzip -o -q csdpmi7b.zip -d csdpmi
        rm csdpmi7b.zip
        cd ../..
    fi
    
    echo "Setup complete"
    ;;

"allegro")
    echo "Building Allegro 4 for DJGPP..."
    
    ./build_dos.sh setup
    
    if [ ! -d "$BUILD_DIR/source-install" ]; then
        echo "Downloading DJGPP-compatible Allegro 4 source..."
        cd $BUILD_DIR
        
        # Use the DJGPP cross-compilation fork instead
        curl -L -o allegro-4.2.3.1-xc.tar.gz https://github.com/superjamie/allegro-4.2.3.1-xc/archive/refs/heads/master.tar.gz
        cd ../..
        
        echo "Building Allegro 4 in container..."
        docker run --rm \
            -v $(pwd)/$BUILD_DIR:/workspace:z \
            -w /workspace \
            --user root \
            $DJGPP_IMAGE \
            /bin/bash -c "
                tar xzf allegro-4.2.3.1-xc.tar.gz
                cd allegro-4.2.3.1-xc-master
                
                # Build using the xmake script (DJGPP-specific)
                chmod +x xmake.sh
                ./xmake.sh lib
                
                # Create install directory structure
                mkdir -p /workspace/source-install/include
                mkdir -p /workspace/source-install/lib
                
                # Copy headers
                cp -r include/* /workspace/source-install/include/
                
                # Copy library
                cp lib/djgpp/liballeg.a /workspace/source-install/lib/
                
                chown -R $USER_ID:$GROUP_ID /workspace
                echo 'Allegro 4 built successfully with DJGPP cross-compilation fork'
            "
    else
        echo "Allegro 4 already built"
    fi
    ;;

"dos")
    echo "Building WarpNES Emulator for DOS..."
    
    # Ensure Allegro is built
    ./build_dos.sh allegro
    
    # Check source files
    if [ ! -f "source/dos_main.cpp" ]; then
        echo "ERROR: source/dos_main.cpp not found!"
        exit 1
    fi
    
    # Build with Allegro 4 using the working one-line compilation approach
    echo "Compiling with DJGPP and Allegro 4..."
    docker run --rm \
        -v $(pwd):/src:z \
        -u $USER_ID:$GROUP_ID \
        $DJGPP_IMAGE \
        /bin/sh -c "
            cd /src && 
            echo 'Checking available libraries...' &&
            find $BUILD_DIR/source-install -name '*.a' 2>/dev/null || echo 'No .a files found' &&
            echo 'Creating object directory...' &&
            mkdir -p /src/$BUILD_DIR/obj &&
            echo 'Compiling individual source files...' &&
            g++ -c /src/source/Configuration.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Configuration.o && \
            g++ -c /src/source/Emulation/APU.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/APU.o && \
            g++ -c /src/source/Emulation/AllegroMidi.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/AllegroMidi.o && \
            g++ -c /src/source/Emulation/Controller.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Controller.o && \
            g++ -c /src/source/Emulation/PPU.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/PPU.o && \
            g++ -c /src/source/SMB/SMBEmulator.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/SMBEngine.o && \
            g++ -c /src/source/SMB/Battery.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Battery.o && \
            g++ -c /src/source/Zapper.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Zapper.o && \
            g++ -c /src/source/dos_main.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Main.o && \
            g++ /src/$BUILD_DIR/obj/*.o -L/src/$BUILD_DIR/source-install/lib -lalleg -lm -O3 -march=i586 -s -o /src/$BUILD_DIR/warpnese.exe &&
            echo 'Converting to COFF format...' &&
            exe2coff /src/$BUILD_DIR/waprnese.exe &&
            echo 'Creating final DOS executable with DPMI stub...' &&
            exe2coff /src/$BUILD_DIR/warpnese.exe &&
            cat /src/$BUILD_DIR/csdpmi/bin/CWSDSTUB.EXE /src/$BUILD_DIR/warpnese > /src/$BUILD_DIR/warpnes.exe &&
            #rm /src/$BUILD_DIR/warpnese.exe
            echo 'DOS build complete!'
        "
    
    # Copy DPMI server (find it wherever it is)
    echo "Looking for DPMI files..."
    ls -la $BUILD_DIR/csdpmi/ 2>/dev/null || echo "CSDPMI directory not found"
    find $BUILD_DIR -name "CWSDPMI.EXE" -o -name "cwsdpmi.exe" -o -name "CWSDSTUB.EXE" 2>/dev/null
    
    DPMI_FILE=$(find $BUILD_DIR -name "CWSDPMI.EXE" -o -name "cwsdpmi.exe" 2>/dev/null | head -1)
    if [ -n "$DPMI_FILE" ]; then
        cp "$DPMI_FILE" $BUILD_DIR/
        echo "Copied DPMI server: $(basename "$DPMI_FILE")"
    else
        echo "Warning: DPMI server not found - DOS executable may need DPMI host"
    fi
    
    echo ""
    echo "🎮 WarpNES Emulator built with Allegro 4!"
    echo "📁 Files:"
    #rm $BUILD_DIR/warpnese.exe
    ls -la $BUILD_DIR/*.exe $BUILD_DIR/*.EXE 2>/dev/null || true
    ;;

"compile")
    echo "Quick compile (assumes Allegro is already built)..."
    
    # Check if Allegro is built
    if [ ! -d "$BUILD_DIR/source-install" ]; then
        echo "Allegro not found. Building first..."
        ./build_dos.sh allegro
    fi
    
    # Check source files
    if [ ! -f "source/dos_main.cpp" ]; then
        echo "ERROR: source/dos_main.cpp not found!"
        exit 1
    fi
    
    # Quick compile using the working one-line approach
    echo "Quick compiling with DJGPP and Allegro 4..."
    docker run --rm \
        -v $(pwd):/src:z \
        -u $USER_ID:$GROUP_ID \
        $DJGPP_IMAGE \
        /bin/sh -c "
            cd /src && 
            echo 'Checking available libraries...' &&
            find $BUILD_DIR/source-install -name '*.a' 2>/dev/null || echo 'No .a files found' &&
            echo 'Creating object directory...' &&
            mkdir -p /src/$BUILD_DIR/obj &&
            echo 'Compiling individual source files...' &&
            g++ -c /src/source/Configuration.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Configuration.o && \
            g++ -c /src/source/Emulation/APU.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/APU.o && \
            g++ -c /src/source/Emulation/AllegroMidi.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/AllegroMidi.o && \
            g++ -c /src/source/Emulation/Controller.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Controller.o && \
            g++ -c /src/source/Emulation/PPU.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/PPU.o && \
            g++ -c /src/source/SMB/SMBEmulator.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/SMBEngine.o && \
            g++ -c /src/source/SMB/Battery.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Battery.o && \
            g++ -c /src/source/Zapper.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Zapper.o && \
            g++ -c /src/source/dos_main.cpp -I/src/$BUILD_DIR/source-install/include -O3 -march=i586 -fomit-frame-pointer -ffast-math -funroll-loops -fpermissive -w -o /src/$BUILD_DIR/obj/Main.o && \
            g++ /src/$BUILD_DIR/obj/*.o -L/src/$BUILD_DIR/source-install/lib -lalleg -lm -O3 -march=i586 -s -o /src/$BUILD_DIR/warpnese.exe &&
            echo 'Converting to COFF format...' &&
            exe2coff /src/$BUILD_DIR/waprnese.exe &&
            echo 'Creating final DOS executable with DPMI stub...' &&
            exe2coff /src/$BUILD_DIR/warpnese.exe &&
            cat /src/$BUILD_DIR/csdpmi/bin/CWSDSTUB.EXE /src/$BUILD_DIR/warpnese > /src/$BUILD_DIR/warpnes.exe &&
            rm /src/$BUILD_DIR/warpnese.exe
            echo 'Quick compile complete!'
        "
    rm $BUILD_DIR/smb -f
    echo "🎮 WarpNES Emulator compiled!"
    echo "📁 Files:"
    ls -la $BUILD_DIR/* 2>/dev/null || true
    ;;

"run")
    if [ ! -f "$BUILD_DIR/warpnes.exe" ]; then
        echo "WarpNES Emulator not built yet. Building..."
        ./build_dos.sh dos
    fi
    
    echo "Running WarpNES Emulator in DOSBox..."
    cd $BUILD_DIR && dosbox warpnes.exe
    ;;

"clean")
    echo "Cleaning build files..."
    rm -rf $BUILD_DIR
    echo "Clean complete"
    ;;

"clean-objects")
    echo "Cleaning object files only..."
    rm -rf $BUILD_DIR/obj
    echo "Object files cleaned"
    ;;

*)
    echo "Usage: $0 [setup|allegro|dos|compile|run|clean|clean-objects]"
    echo ""
    echo "Commands:"
    echo "  setup       - Download and setup DJGPP environment and CSDPMI"
    echo "  allegro     - Build Allegro 4 library for DJGPP"
    echo "  dos         - Full build (setup + allegro + compile)"
    echo "  compile     - Quick compile (assumes Allegro is built)"
    echo "  run         - Run the built executable in DOSBox"
    echo "  clean       - Remove all build files"
    echo "  clean-objects - Remove only object files"
    ;;

esac
