### [Pintos](https://casys-kaist.github.io/pintos-kaist/)

- 운영체제 kernel의 자원 관리 방법을 프로그래밍 하는 과제, 약 4주간(24.04.27 ~ 24.05.27) 개발
- C를 사용하여 pintos 과제 중 threads, userprog, virtual memory, filesys 전부 구현[192/193](buffer cache제외)
- 각 주차별 핵심 기능을 구현할 때 최적의 자료 구조를 찾기 위해 다양한 시도를 함
    1. thread가 우선 순위에 따라 실행되는 스케줄러 구현
        1. 스케줄러의 순서를 저장하는 구조로 메모리를 줄이기 위해 Linked list대신 array로 구현
    2. open, dup2 syscall을 구현하기 위해 fd를 저장할 File discriptor table과 File table(file entry table)을 구현
        1. FDT와 FET 각 페이지에 최소 메모리를 사용하고 무한히 확장 가능한 구조 선택
        2. paged array를 linked list로 연결한 구조를 사용하여 fd를 무한히 추가 가능하게 함
        3. 각 페이지 접근 시 빈 array를 찾는 속도를 높이기 위해 최소,최대의 index를 저장하여 사용
    3. 가상 메모리의 Copy on write를 구현 하기 위해 프레임의 fork된 횟수를 reference count로 구현
        1. swap out 시 프레임이 삭제되기 때문에 ref count로 구현 불가, 중복 참조 페이지를 circular linked list로 연결하여 관리
        2. swap out & in 할 때 중복 참조 페이지를 순환하면서 연결 해제, eviction 시 비용이 높아 clock algorithm에서 후순위로 설정
    4. 파일 시스템의 동기화 속도를 최적화, 파일이 증가하는 경우에만 lock을 걸어 read가 blocking되는 시간을 최소화하도록 구현

#### outcome
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
