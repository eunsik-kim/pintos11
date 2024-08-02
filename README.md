# [Pintos](https://casys-kaist.github.io/pintos-kaist/)

- 운영체제 kernel의 자원 관리 방법을 프로그래밍 하는 과제, 약 4주간(24.04.27 ~ 24.05.27) 개발
- C를 사용하여 pintos 과제 중 threads, userprog, virtual memory, filesys 전부 구현[192/193](buffer cache제외)

## 내부 구조
### QEMU emulator 실행
1. OS 시작에 필요한 모든 설정(thread, malloc, paging, timer etc..) 초기화


### Thread Life Cycle

1. **Thread Kernel Page 할당 및 초기화**
    - Thread의 커널 페이지를 할당하고, interrupt frame과 기본 값을 설정합니다.
  
2. **Thread Function 실행**
    - 특정 thread function을 실행하여 프로그램이 실행되거나 특정 동작을 수행합니다.

3. **Thread 양보(문맥 전환)**
    - Ready queue(linked list)에 존재하는 thread와 현재 실행 중인 thread를 교체합니다.

    1. **문맥 전환 과정**
        - 현재 thread의 interrupt frame에 register 값을 저장하고, 교체할 thread의 정보를 현재 register로 로드합니다.
    
    2. **문맥 전환 과정**
        - 가상 메모리 설정: 교체할 thread의 pml4를 설정합니다.
        - 커널 영역 진입 설정: tss에 교체할 thread 주소를 저장합니다.
    
    3. **양보의 경우**
        1. **Case 1: Timer Tick 초과**
            - Timer tick이 3 tick을 넘는 경우 양보 (선점 방지).

        2. **Case 2: 우선순위에 따른 양보**
            - 높은 우선순위의 thread가 ready queue에 존재하는 경우 양보.

        3. **Case 3: 우선순위 기부**
            - Blocked된 우선순위가 높은 thread를 깨우기 위해 우선순위 기부를 받은 경우, 높은 우선순위 thread를 깨우고 양보.

4. **Thread 종료**
    - Thread function 함수 실행 종료 후 사용한 자원을 정리합니다 (page, frame 반환, pml4 해제, fdt 삭제).

### 프로세스 생성 
Thread 생애 주기 2번으로 실행되거나 exec 호출 이후 과정입니다.

1. **메모리 매핑 페이지가 존재하는 경우**
    - 메모리 매핑 페이지는 프로세스 재실행 하더라도 사라지지 않기 때문에 새로운 프로세스로 페이지를 이전합니다.

2. **새롭게 실행할 프로세스의 파일 이름과 arguments 분리**

3. **프로그램 로딩**
    - 분리된 파일 이름에 해당하는 프로그램을 디스크의 디렉토리 안에서 찾아 로딩합니다.

4. **프로그램의 각 섹션 초기화 (for lazy loading)**
    1. 코드 섹션은 file-backed page로 설정하고, BSS 섹션은 anonymous page로 설정합니다.
    2. 스택 영역은 anonymous page로 설정합니다.

5. **새로운 스레드로 문맥 전환**

### Demand Paging (Lazy Loading 포함)

1. **페이지 폴트 발생 시 유효성 검증**
2. **유효한 경우 물리 메모리 할당**
    - 새 프레임을 연결하고 PML4(가상 메모리의 multi-level page table)를 설정합니다.
3. **프레임 할당**
    1. 여유 프레임이 없는 경우: 할당된 프레임 중 clock 알고리즘으로 희생양을 찾아 빈 프레임을 생성합니다.
        1. 희생양을 찾으면 swap out을 실행합니다: anonymous page는 swap disk로, file page는 디스크의 inode에 기록합니다.
    2. 여유 프레임이 있는 경우: swap in을 실행하여 프레임에 파일의 데이터 혹은 swap disk의 데이터를 물리 메모리에 적재합니다.

### System Call 호출 과정

1. **User Program에서 System Call 호출**
2. **rax에 syscall number 설정 후 syscall-entry.S 실행**
    1. **커널 영역 진입**
        - 현재 rsp에 커널 스택의 rsp(tss 값)를 설정합니다.
    2. **현재 정보 저장**
        - 커널 스택에 현재 CPU의 레지스터 값을 interrupt frame 구조체 순서대로 저장합니다.
3. **syscall_handler 실행**
    - rax의 값을 확인하여 syscall number에 해당하는 함수를 실행합니다.

### Fork Call 과정 (부모 프로세스의 자원을 자식 프로세스에 동일하게 복사)

1. **자식 스레드 생성 후 실행**
    - 부모 스레드는 자식 스레드가 fork를 완전히 수행하기 전까지 block(sema down) 상태가 됩니다.
2. **부모 스레드의 자원 복사**
    1. SPT(supplemental page table), cwd(current working director), FDT(file descriptor table) 전부 복사합니다.
    2. Copy-on-Write : 복사한 페이지의 프레임은 실제로 사용할 때 (demand paging) 복사합니다.
3. **부모 스레드 unblock(sema up) 후 문맥 전환**

### FAT(File Allocation Table) 파일 시스템의 파일 열고, 읽고 쓰기

#### Open

1. **file open syscall 실행**
2. **파일 경로 확인**
    1. 상대 경로: 현재 폴더에서 파일의 inode를 찾습니다.
    2. 절대 경로: 재귀적으로 디렉토리를 탐색한 뒤 마지막 디렉토리에서 파일의 inode를 찾습니다.
3. **파일 객체 생성**
    - 찾은 inode를 포함하는 파일 객체를 생성하여 file entry table에 기록합니다.
4. **파일 디스크립터 생성**
    - 파일 객체를 가리키는 fd를 생성하여 file descriptor table에 기록한 후 fd를 반환합니다.

#### Read / Write

1. **fd를 이용하여 파일의 시작 inode를 찾음**
2. **inode의 클러스터를 순회하여 inode를 찾음**
3. **FAT 시스템에서 디스크 섹터를 찾음**
4. **ATA channel을 통해 디스크 섹터를 읽기/쓰기**
    1. 읽을 위치 전달: 디스크의 채널에 lock을 걸고 디스크 섹터를 전달합니다.
    2. 인터럽트를 켜서 채널의 포트를 통해 읽을 준비가 됨을 알립니다.
        1. 원자성 보장: block(sema down)되어 대기 후, interrupt handler에 의해 unblock됩니다.
        2. busy waiting: 디스크가 읽을 준비가 될 때까지 sleep & wait를 반복합니다.
    3. 디스크의 채널이 통신할 준비가 된 경우 채널의 포트를 통해 buffer에 데이터를 적재합니다.
    4. lock 해제.
5. **파일 쓰기의 파일 크기 증가 과정**
    1. 추가할 파일의 크기가 디스크 섹터 크기를 초과하는 경우 클러스터를 연장합니다.
    2. 추가할 파일의 크기가 디스크 섹터 크기를 초과하지 않는 경우 파일 길이만 연장합니다.
    3. 파일의 길이 변동을 파일 inode에 기록합니다.
    4. 파일의 늘어난 섹터를 0으로 초기화합니다.

### 메모리 최적화

각 주차별 핵심 기능을 구현할 때 최적의 자료 구조를 찾기 위해 다양한 시도를 하였습니다.
    
1. **스레드가 우선순위에 따라 실행되는 스케줄러 구현**
    1. 스케줄러의 donation 순서를 저장하는 구조로 메모리를 줄이기 위해 linked list 대신 array로 구현했습니다.
    2. 하지만 코드 구조가 복잡하여 수정에 용이한 linked list로 재구현 하였습니다.
    
2. **open, dup2 syscall을 구현하기 위해 fd를 저장할 File Descriptor Table과 File Table(File Entry Table) 구현**
    1. FDT와 FET를 저장할 공간을 선택하기 위해 linked list혹은 array를 고려할 수 있습니다. 메모리를 줄이기 위해 array를 선택하였습니다. 
    2. 수평적 확장을 위해 사용하는 array page를 linked list로 연결하게 하였습니다. 
    3. 각 페이지 접근 시 free index 찾는 속도를 높이기 위해 최소, 최대의 index를 저장하여 사용했습니다.
    4. page 별로 관리를 하였기에 fdt는 fork시 linked list보다 훨신 빠르게 copy 가능하였습니다. (fet는 동일)
    
3. **가상 메모리의 Copy-on-Write를 구현**
    1. fork할 때 할당된 가상 메모리 복사를 구현하기 위해 처음엔 fork된 횟수(reference count)를 frame 안에 기록하여 구현하엿습니다.
    2. 그렇게 하였더니 swap out시 프레임이 삭제되어 reference count로 구현이 불가능하였습니다.
    3. 그래서 fork할 때 같은 frame을 참조하는 페이지들은 circular linked list로 연결하여 관리하였고, swap out하는 경우 모든 page들을 한번에 swap out하도록 하였습니다.
    4. 뿐만 아니라 eviction 시 비용이 높으므로 clock algorithm을 약간 수정하여 fork된 page는 eviction할 때 후순위로 설정하였습니다.


## outcome
project1 -  THREADS

<img src="https://github.com/eunsik-kim/pintos11/assets/153556378/a77c6e1a-0678-4547-9bbe-b803fe87acfb" alt="project1" width="300" align="center">

project2 - USER PROGRAMS

<img src="https://github.com/eunsik-kim/pintos11/assets/153556378/388c2f7a-806b-465c-abc8-ba1e73e35131" alt="project1" width="300" align="center">

project3 -  VIRTUAL MEMORY

<img src="https://github.com/eunsik-kim/pintos11/assets/153556378/e1caf7cc-3460-47f3-9027-8f2570c03c7b" alt="project1" width="300" align="center">

project4 - FILESYS

<img src="https://github.com/eunsik-kim/pintos11/assets/153556378/b7b3115c-df9a-4142-8ce4-7cd196b39775" alt="project1" width="300" align="center">

watch detail implementaion
https://eunsik-kim.github.io/posts/WIC-week11-Pintos-FileSystems/
